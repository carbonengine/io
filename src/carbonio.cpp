#include <carbonio.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "socketmodule.h"
#include "protocol.h"

#ifndef INVALID_SOCKET /* MS defines this */
#define INVALID_SOCKET ( -1 )
#endif

SchedulerCAPI* g_scheduler{nullptr};

// This is the module name that shows up in loglite.
const char* g_moduleName = "_socket";

static_assert( sizeof( SOCKET_T ) == sizeof( uv_os_fd_t ), "Mismatching size between SOCKET_T and uv_os_fd_t" );

#if __APPLE__
// AppleClang doesn't know the _s versions yet, so we are forced to do the unsafe thing
#ifndef __STDC_LIB_EXT1_
#define memcpy_s(dst, dstsize, src, srcsize) memcpy(dst, src, srcsize)
#define memmove_s(dst, dstsize, src, srcsize) memmove(dst, src, srcsize)
#endif
// Same for ULONG, which we need to use with libuv on Windows
#ifndef ULONG
typedef unsigned int ULONG;
#endif
#endif

static uv_key_t s_tlsKey;

static std::atomic_size_t s_bytesReceived{0};
static std::atomic_size_t s_bytesSent{0};
static std::atomic_size_t s_packetsReceived{0};
static std::atomic_size_t s_packetsSent{0};

static std::vector<OobDataCallback> s_oobDataCallbacks{};

static std::unordered_map<SOCKET_T, uv_handle_t*> s_uvHandleLookup;
static std::mutex s_uvHandleLookupLock;

void AddToLookupTable( SOCKET_T fileDescriptor, uv_handle_t* uvHandle )
{
	std::scoped_lock mutex( s_uvHandleLookupLock );
	s_uvHandleLookup[fileDescriptor] = uvHandle;
}

uv_handle_t* LookupHandle( SOCKET_T fileDescriptor )
{
	std::scoped_lock mutex( s_uvHandleLookupLock );
	auto iter = s_uvHandleLookup.find( fileDescriptor );
	if ( iter != s_uvHandleLookup.cend() )
	{
		return iter->second;
	}

	return nullptr;
}

void RemoveFromLookupTable( SOCKET_T fileDescriptor )
{
	std::scoped_lock mutex( s_uvHandleLookupLock );
	auto iter = s_uvHandleLookup.find( fileDescriptor );
	if ( iter != s_uvHandleLookup.cend() )
	{
		s_uvHandleLookup.erase( iter );
	}
}

// Simple wrapper function so that we can use std::shared_ptr
void DeleteRequestQueueChannel(PyChannelObject* obj)
{
	if ( obj )
	{
		// Ensure nobody is left waiting
		auto balance = g_scheduler->PyChannel_GetBalance( obj );
		while ( g_scheduler->PyChannel_GetBalance( obj ) < 0 ) {
			g_scheduler->PyChannel_Send( obj, Py_None );
		}
		Py_DecRef( (PyObject*) obj );
	}
}

PyObject* GetStatistics()
{
	auto ret = Py_BuildValue("{sL sL sL sL}",
		"BytesReceived", s_bytesReceived.load(),
		"BytesSent", s_bytesSent.load(),
		"PacketsReceived", s_packetsReceived.load(),
		"PacketsSent", s_packetsSent.load()
	);
	return ret;
}

bool InitScheduler()
{
	g_scheduler = SchedulerAPI();
	return g_scheduler != nullptr;
}

int InitUvLoop() {
	// uv_loop instances aren't thread-safe, thus we keep a loop instance per thread for which we need to initialize TLS
	auto status = uv_key_create(&s_tlsKey);
	if ( status != 0 ) {
		PyErr_FromUvErr( status );
	}
	return status;
}

void TickUvLoop()
{
	uv_run(GetUvLoop(), UV_RUN_NOWAIT);
}

void cleanup_uv_handle( uv_handle_t* uv_handle )
{
	Ccp::PyGilEnsure gil;
	auto data = reinterpret_cast<HandleData*>( uv_handle->data );
	if( data )
	{
		if ( data->request ) {
			data->request->cancel();
		}
		if ( data->receiveRequest )
		{
			data->receiveRequest->cancel();
		}
		delete data;
	}
	uv_handle->data = nullptr;

	switch( uv_handle_get_type( uv_handle ) )
	{
	case UV_TCP:
		delete reinterpret_cast<uv_tcp_t*>(uv_handle);
		break;
	case UV_UDP:
		delete reinterpret_cast<uv_udp_t*>(uv_handle);
		break;
	default:
		delete uv_handle;
		break;
	}
}

bool is_valid_uv_handle( uv_handle_t* handle )
{
	return handle && !uv_is_closing( handle );
}

HandleData::HandleData() : channel( g_scheduler->PyChannel_New( nullptr ) ), receiveQueue( g_scheduler->PyChannel_New( nullptr ), DeleteRequestQueueChannel ), sendQueue( g_scheduler->PyChannel_New( nullptr ), DeleteRequestQueueChannel )
{
	buf = uv_buf_init( nullptr, 0 );
}

HandleData::~HandleData()
{
	Py_XDECREF( channel );
	channel = nullptr;
	receiveQueue = nullptr;
	delete buf.base;
	buf.base = nullptr;
	buf.len = 0;
	bufReadPos = -1;
	bufWritePos = -1;
	socket = nullptr;
}


void* CreateHandleData()
{
	auto* data = new HandleData;
	if( data->channel == nullptr )
	{
		delete data;
		return nullptr;
	}
	if( data->receiveQueue == nullptr )
	{
		Py_DECREF( data->channel );
		delete data;
		return nullptr;
	}

	g_scheduler->PyChannel_SetPreference( data->receiveQueue.get(), PREFER_SENDER );
	return data;
}

void PyErr_FromUvErr( int uv_status )
{
	auto errnoModule = PyImport_ImportModule("errno");
	auto errnoObj = PyObject_GetAttrString( errnoModule, uv_err_name( uv_status ) );
	errno = PyLong_AsLong( errnoObj );
	PyObject* exc_type = PyExc_OSError;
	if ( errno == EWOULDBLOCK ) {
		exc_type = PyExc_BlockingIOError;
	}
	PyErr_SetFromErrno( exc_type );
}

static std::string FormatTraceback(PyObject* tb)
{
	std::string result = "Traceback (most recent call first):\n";
	while (tb && PyObject_IsTrue(tb) ){
		std::string filename = "<none>";
		PyObjectPtr frame(PyObject_GetAttrString(tb, "tb_frame"));
		if (frame) {
			PyObjectPtr code(PyObject_GetAttrString(frame.get(), "f_code"));
			if (code) {
				PyObjectPtr fname(PyObject_GetAttrString(code.get(), "co_filename"));
				if (fname)
					filename = std::string(PyUnicode_AsUTF8(fname.get()));
			}
		}
		PyErr_Clear();
		int line = 0;
		PyObjectPtr lineno(PyObject_GetAttrString(tb, "tb_lineno"));
		if (lineno)
		{
			line = int( PyLong_AsLong( lineno.get() ) );
		}
		PyErr_Clear();
		result += filename + ":" + std::to_string( line ) + "\n";
		tb = PyObject_GetAttrString(tb, "tb_next");
	}
	result += "Traceback end\n";
	PyErr_Clear();
	return result;
}

std::string FormatException(PyObject* exc, PyObject* val, PyObject* tb)
{
	std::string result = "Exception start:\n";
	PyObjectPtr typeString( PyObject_Repr(exc) );
	if (exc)
	{
		result += std::string( "Type: " ) + PyUnicode_AsUTF8( typeString.get() ) + "\n";
	}
	if (val) {
		PyObjectPtr valueString( PyObject_Repr(val) );
		if (valueString)
		{
			result += std::string( "Value: " ) + PyUnicode_AsUTF8( valueString.get() ) + "\n";
		}
	}
	if (tb)
	{
		result += FormatTraceback( tb );
	}
	result += "Exception end\n";
	PyErr_Clear();
	return result;
}

void LogError( const char* msg )
{
	if( !PyErr_Occurred() )
	{
		CCP_LOGERR(msg);
		return;
	}
	PyObject *exc, *val, *tb;
	PyErr_Fetch( &exc, &val, &tb );
	auto errorString = std::string(msg) + "\n\n" + FormatException(exc, val, tb);
	CCP_LOGERR(errorString.c_str());
	PyErr_Restore(exc, val, tb);
}

uv_loop_t * GetUvLoop()
{
	uv_loop_t* ret = reinterpret_cast<uv_loop_t*>(uv_key_get(&s_tlsKey));
	if ( !ret ) {
		ret = new uv_loop_t;
		auto res = uv_loop_init( ret );
		if ( res < 0 ) {
			uv_loop_delete( ret );
			Ccp::PyGilEnsure gil;
			PyErr_FromUvErr( res );
			return nullptr;
		}
		uv_key_set( &s_tlsKey, reinterpret_cast<void *>( ret ) );
	}
	return ret;
}

static PyObject* s_timeout_error;
void SetTimeoutErrorType( PyObject* value )
{
	s_timeout_error = value;
}

IRequest::IRequest( PySocketSockObject* socket ) : m_handle( socket->uv_handle ), m_timeout_nanoseconds(socket->sock_timeout), m_requestQueue( handleData()->receiveQueue ), m_sendQueue( handleData()->sendQueue )
{
	m_channel = g_scheduler->PyChannel_New( nullptr );
	if( !m_channel )
	{
		LogError( "Failed to create channel for request" );
	}
	else
	{
		g_scheduler->PyChannel_SetPreference( m_channel, PREFER_SENDER );
	}
	g_scheduler->PyChannel_SetPreference( m_requestQueue.get(), PREFER_SENDER );
}

void IRequest::acquireReceive(const char* context)
{
	auto keepAlive = shared_from_this();

	// there's already an outstanding request associated with the socket, let's wait until we can perform our operation
	while ( handleData()->receiving ) {
		// something is already reading, so let's try again at a later point in time
		auto sentinel = g_scheduler->PyChannel_Receive( m_requestQueue.get() );
		if ( !sentinel ) {
			LogError("Oh boy");
		}
	}
	handleData()->receiving = true;
	handleData()->receiveRequest = this->shared_from_this();
}

void IRequest::acquireSend( const char* context )
{
	auto keepAlive = shared_from_this();

	// there's already an outstanding request associated with the socket, let's wait until we can perform our operation
	while ( handleData()->sending ) {
		// something is already reading, so let's try again at a later point in time
		auto sentinel = g_scheduler->PyChannel_Receive( m_sendQueue.get() );
		if ( !sentinel ) {
			LogError("Oh boy");
		}
	}
	handleData()->sending = true;
	handleData()->request = this->shared_from_this();
}


void IRequest::associateWithHandleData()
{
	handleData()->request = this->shared_from_this();
}

void IRequest::releaseReceive(const char* context)
{
	handleData()->receiving = false;
	handleData()->receiveRequest = nullptr;
	auto balance = g_scheduler->PyChannel_GetBalance( m_requestQueue.get() );
	if ( balance < 0 ) {
		if ( g_scheduler->PyChannel_Send( m_requestQueue.get(), Py_None ) < 0 ) {
			LogError( "IRequest::releaseReceive() failed to signal waiting handlers" );
			PyErr_Format( PyExc_SystemError, "IRequest::releaseReceive() failed to signal waiting handlers" );
		}
	}
}

void IRequest::releaseSend(const char* context)
{
	handleData()->sending = false;
	handleData()->request = nullptr;
	auto balance = g_scheduler->PyChannel_GetBalance( m_sendQueue.get() );
	if ( balance < 0 ) {
		if ( g_scheduler->PyChannel_Send( m_sendQueue.get(), Py_None ) < 0 ) {
			LogError( "IRequest::releaseSend() failed to signal waiting handlers" );
			PyErr_Format( PyExc_SystemError, "IRequest::releaseSend() failed to signal waiting handlers" );
		}
	}
}

void IRequest::sendError(std::string_view msg)
{
	PyObject *exc, *val, *tb;
	PyErr_Fetch( &exc, &val, &tb );
	g_scheduler->PyChannel_SetPreference( m_channel, PREFER_SENDER );
	auto ret = g_scheduler->PyChannel_SendThrow( m_channel, exc, val, tb);
	if( ret < 0 )
	{
		// we intentionally do not restore the exception state here, otherwise it may end up in `socket.dispatch()` where it isn't very useful
		LogError( msg.data() );
	}
}

void IRequest::timeoutCallback( uv_timer_t* result )
{
	// The timer always holds a pointer to the request
	auto _this = reinterpret_cast<IRequest*>( result->data );
	_this->onTimeout();
}

PyObject* IRequest::startTimeout()
{
	// Python differentiates between three kinds of socket operations:
	// Blocking: these are operations on sockets that never experienced a `settimeout(None)` or `setblocking(True)` call, internally their blocking value is `Py_None`
	// Non-blocking: these are operations on sockets for which either `setblocking(False)` or `settimeout(0.0)` were called. Internally this is represented by a timeout value of `0`
	// Blocking, but with a timeout: these are socket on which `setttimeout(val)` was called with a `val` larger than 0.
	//
	// Since libuv does not support that concept directly, the IRequest class maps this behaviour using its `m_timeout_nanoseconds` value.
	// Blocking: `m_timeout_nanoseconds == -1;`
	// Non-blocking: `m_timeout_nanoseconds == 0;`
	// Blocking, but with a timeout: `m_timeout_nanoaseconds > 0;`
	//
	// And even though there are no real "non-blocking" operations in libuv, then this mapping simulates such behaviour via `IRequest`'s timeout mechanism.
	if( m_timeout_nanoseconds < 0 )
	{
		Py_RETURN_NONE;
	}

	uint64_t timeout_ms = m_timeout_nanoseconds / 1000000;
	m_timeout = new uv_timer_t;
	m_timeout->data = this;
	uv_timer_init( GetUvLoop(), m_timeout );
	auto status = uv_timer_start(m_timeout, timeoutCallback, timeout_ms, 0);
	if ( status < 0 ) {
		PyErr_FromUvErr( status );
		return nullptr;
	}
	Py_RETURN_NONE;
}

void IRequest::onTimeout()
{
	Ccp::PyGilEnsure gil;
	PyErr_SetString( s_timeout_error, "timed out" );
	m_timedOut = true;
	sendError("IRequest::onTimeout failed to send timeout exception");
}

void IRequest::cancel()
{
	clearTimeout();
}

void IRequest::clearTimeout()
{
	if( m_timeout )
	{
		uv_timer_stop( m_timeout );
		m_timeout->data = nullptr;
		uv_close( reinterpret_cast<uv_handle_t*>( m_timeout ), cleanup_uv_handle);
		m_timeout = nullptr;
	}
}


PyObject* StreamRecvRequest::execute()
{
	acquireReceive("StreamRecvRequest");
	ON_BLOCK_EXIT([&]{releaseReceive("StreamRecvRequest");});
	auto* data = handleData();

	auto bufferedAmount = data->bufWritePos - data->bufReadPos;

	if ( m_requested_len > bufferedAmount )
	{
		auto ret = startTimeout();
		if( ret != Py_None )
		{
			return nullptr;
		}
		auto status = startRead();
		if( status < 0 )
		{
			PyErr_FromUvErr( status );
			return nullptr;
		}
		auto sentinel = g_scheduler->PyChannel_Receive( m_channel );
		if( !sentinel )
		{
			return nullptr;
		}
	}
	data->bufWritePos += m_received_len;
	s_bytesReceived += m_received_len;

	return constructResult( data );
}

PyObject* StreamRecvRequest::constructResult( HandleData* data ) const
{
	ssize_t bufferedAmount = data->bufWritePos - data->bufReadPos;
	auto chunkSize = bufferedAmount < m_requested_len ? bufferedAmount : m_requested_len;
	auto* ret =  PyBytes_FromStringAndSize(data->buf.base + data->bufReadPos, chunkSize);
	data->bufReadPos += chunkSize;
	return ret;
}

void StreamRecvRequest::onCallback( ICallbackParams* callbackParams )
{
	if ( m_timedOut )
	{
		return;
	}
	Ccp::PyGilEnsure gil;
	ON_BLOCK_EXIT( [&] { clearTimeout(); } );
	auto params = dynamic_cast<StreamRecvRequest::Params*>( callbackParams );
	ssize_t nread = params->nread;
	if( nread == 0 ) {
		return;
	}
	g_scheduler->PyChannel_SetPreference( m_channel, PREFER_SENDER );
	if ( nread < 0 ) {
		if ( nread != UV_EOF ) {
			PyErr_FromUvErr( int( nread ) );
			sendError( "OnReceive failed to read data." );
		}
		else {
			if ( g_scheduler->PyChannel_Send( m_channel, Py_None ) < 0 ) {
				LogError( "StreamRecvRequest::onReceive failed to signal sentinel" );
				PyErr_Clear();
			}
		}
	}
	if ( nread > 0 ) {
		m_received_len += nread;
		uv_read_stop( handle() );
		if ( g_scheduler->PyChannel_Send( m_channel, Py_None ) < 0 ) {
			LogError( "StreamRecvRequest::onReceive failed to signal sentinel" );
			PyErr_Clear();
		}
	}
}

void growingBufferAlloc(uv_handle_t* handle, size_t size, uv_buf_t* buf)
{
	auto* data = reinterpret_cast<HandleData*>(handle->data);
	auto& handleBuf = data->buf;

	constexpr size_t BUF_SIZE = 4096;

	// Scenario 1: We don't have a buffer yet, allocate one.
	if( !handleBuf.base )
	{
		handleBuf.base = new char[BUF_SIZE];
		handleBuf.len = BUF_SIZE;

		buf->base = handleBuf.base;
		buf->len = handleBuf.len;
		return;
	}

	// Scenario 2: We have a buffer, but we have read everything.
	// Just use it completely.
	auto unreadBytes = data->bufWritePos - data->bufReadPos;
	if( unreadBytes == 0 )
	{
		data->bufReadPos = 0;
		data->bufWritePos = 0;

		buf->base = handleBuf.base;
		buf->len = handleBuf.len;
		return;
	}

	// Scenario 3: We have a buffer with unread data. How much space
	// do we have left in the buffer? If it's very little, we should
	// provide more space.
	auto remainingBytes = handleBuf.len - data->bufWritePos;
	if( remainingBytes > 0 )
	{
		buf->base = handleBuf.base + data->bufWritePos;
		buf->len = handleBuf.len - data->bufWritePos;
		return;
	}

	// Scenario 4: We have no space in the buffer.
	// Let's see if we can free up some space without reallocating
	if( unreadBytes < handleBuf.len )
	{
		memmove_s(handleBuf.base, handleBuf.len, handleBuf.base + data->bufReadPos, unreadBytes);
		data->bufWritePos -= data->bufReadPos;
		data->bufReadPos = 0;

		buf->base = handleBuf.base + data->bufWritePos;
		buf->len = handleBuf.len - data->bufWritePos;
		return;
	}

	// Scenario 5: Still no space in the buffer. Let's give up and cough up some memory.
	char* newBuf = new char[unreadBytes + BUF_SIZE];
	memcpy_s(newBuf, unreadBytes + BUF_SIZE, handleBuf.base + data->bufReadPos, unreadBytes);
	delete handleBuf.base;
	data->bufWritePos -= data->bufReadPos;
	data->bufReadPos = 0;
	handleBuf.base = newBuf;
	handleBuf.len = unreadBytes + BUF_SIZE;

	buf->base = handleBuf.base;
	buf->len = handleBuf.len;
}

void StreamRecvRequest::onTimeout()
{
	uv_read_stop(handle());
	IRequest::onTimeout();
}

void alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf)
{
	// TODO what if allocation fails?!
	buf->base = new char[size];
	buf->len = ULONG(size);
}

void StreamRecvRequest::readCallback( uv_stream_t* client, ssize_t nread, const uv_buf_t* buf )
{
	auto* data = reinterpret_cast<HandleData*>( client->data );
	if( data && data->receiveRequest )
	{
		auto _this = std::reinterpret_pointer_cast<StreamRecvRequest>( data->receiveRequest );
		auto params = StreamRecvRequest::Params( nread, buf );
		_this->onCallback( &params );
	}
}

void StreamRecvRequest::cancel()
{
	uv_read_stop( handle() );
	// Check the balance, as this could be called after the request has finished executing.
	if( g_scheduler->PyChannel_GetBalance( m_channel ) < 0 )
	{
		if( g_scheduler->PyChannel_Send( m_channel, Py_None ) < 0 )
		{
			LogError( "StreamRecvRequest::cancel failed to signal sentinel" );
		}
	}
	IRequest::cancel();
}

StreamRecvRequest::StreamRecvRequest( PySocketSockObject* socket, Py_ssize_t length, int flags ) :
	IStreamRequest( socket ), m_requested_len(length), m_flags(flags)
{
}
int StreamRecvRequest::startRead()
{
	return uv_read_start( handle(), growingBufferAlloc, StreamRecvRequest::readCallback );
}

typedef struct {
	uv_write_t req;
	uv_buf_t buf;
} write_req_t;

// libuv needs a callback
void sendNoopCallback( uv_write_t* req, int )
{
    auto *wr = (write_req_t*) req;
	delete[] wr->buf.base;
	delete wr;
}

PyObject* StreamSendRequest::execute()
{
	auto acquireGuard = MakeGuard( [&] { releaseSend( "StreamSendRequest" ); } );
	auto write_req = new write_req_t;
	write_req->buf = uv_buf_init( new char[m_sendBuffer.len], m_sendBuffer.len );
	memcpy( write_req->buf.base, m_sendBuffer.base, m_sendBuffer.len );
	acquireSend("StreamSendRequest");
	if ( m_blockingSend && ! startTimeout() )
	{
		delete[] write_req->buf.base;
		delete write_req;
		return nullptr;
	}
	auto currentTasklet = reinterpret_cast<PyTaskletObject*>( g_scheduler->PyScheduler_GetCurrent() );
	ON_BLOCK_EXIT( [currentTasklet] {  Py_DECREF(currentTasklet);} );
	if( m_blockingSend && g_scheduler->PyTasklet_GetBlockTrap( currentTasklet ) )
	{
		delete[] write_req->buf.base;
		delete write_req;
		PyErr_SetString(PyExc_RuntimeError, "Can't perform blocking send on a block trapped tasklet");
		return nullptr;
	}
	if( m_blockingSend && g_scheduler->PyTasklet_IsMain( currentTasklet ) )
	{
		delete[] write_req->buf.base;
		delete write_req;
		PyErr_SetString(PyExc_RuntimeError, "Can't perform blocking send on the main tasklet");
		return nullptr;
	}
	int status = uv_write( reinterpret_cast<uv_write_t*>( write_req ), handle(), &write_req->buf, 1, m_blockingSend ? StreamSendRequest::sendCallback : sendNoopCallback );
	if( status < 0 ){
		return PyLong_FromLong(status);
	}
	s_bytesSent += m_sendBuffer.len;

	if( m_blockingSend )
	{
		return g_scheduler->PyChannel_Receive( m_channel );
	}
	return PyLong_FromLong(0);
}

void StreamSendRequest::sendCallback( uv_write_t* request, int status )
{
	if( request->handle && request->handle->data )
	{
		auto _this = std::reinterpret_pointer_cast<StreamSendRequest>( reinterpret_cast<HandleData*>( request->handle->data )->request );
		if( _this )
		{
			auto params = std::make_unique<StreamSendRequest::Params>( status );
			_this->onCallback( params.get() );
		}
	}
    auto *wr = (write_req_t*) request;
	delete[] wr->buf.base;
	delete wr;
}

void StreamSendRequest::onCallback( ICallbackParams* callbackParams )
{
	if( m_timedOut ) // If we have timed out, the execute method has already been unblocked.
	{
		return;
	}
	ON_BLOCK_EXIT( [&] { clearTimeout(); } );
	if ( !m_blockingSend )
	{
		return;
	}
	auto *params = dynamic_cast<StreamSendRequest::Params*>(callbackParams);
	Ccp::PyGilEnsure gil;

	auto py_status = PyLong_FromLong(params->status);
	if( !py_status ){
		sendError("StreamSendRequest::send Failed to convert status to python int");
		return;
	}
	if( g_scheduler->PyChannel_Send( m_channel, py_status ) < 0 )
	{
		LogError( "StreamSendRequest::send Failed to send status over channel" );
		PyErr_Clear();
	}
}

void SendError(PyChannelObject* channel, std::string_view msg)
{
	PyObject *exc, *val, *tb;
	PyErr_Fetch( &exc, &val, &tb );
	auto ret = g_scheduler->PyChannel_SendThrow( channel, exc, val, tb);
	if( ret < 0 )
	{
		PyErr_Restore( exc, val, tb );
		LogError( msg.data() );
	}
}

PyObject* UdpRecvRequest::execute()
{
	acquireReceive("UdpRecvRequest");
	ON_BLOCK_EXIT([&]{releaseReceive("UdpRecvRequest");});
	auto ret = startTimeout();
	if( ret != Py_None )
	{
		return nullptr;
	}

	auto status = uv_udp_recv_start( handle(), alloc, UdpRecvRequest::receiveCallback );
	if ( status < 0 )
	{
		PyErr_FromUvErr( status );
		return nullptr;
	}

	auto sentinel = g_scheduler->PyChannel_Receive( m_channel );
	if( !sentinel )
	{
		return nullptr;
	}

	return PyTuple_Pack( 2, m_buf, m_addr );
}

void UdpRecvRequest::receiveCallback( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags )
{
	auto* data = reinterpret_cast<HandleData*>( handle->data );
	if( data && data->receiveRequest )
	{
		auto _this = std::reinterpret_pointer_cast<UdpRecvRequest>( data->receiveRequest );
		auto params = std::make_unique<UdpRecvRequest::Params>( nread, buf, addr, flags );
		_this->onCallback( params.get() );
	}
}

/* Convert IPv4 sockaddr to a Python str. */

static PyObject*
	make_ipv4_addr( const struct sockaddr_in* addr )
{
	char buf[INET_ADDRSTRLEN];
	if( inet_ntop( AF_INET, &addr->sin_addr, buf, sizeof( buf ) ) == NULL )
	{
		PyErr_SetFromErrno( PyExc_OSError );
		return NULL;
	}
	return PyUnicode_FromString( buf );
}

#ifdef ENABLE_IPV6
/* Convert IPv6 sockaddr to a Python str. */

static PyObject*
	make_ipv6_addr( const struct sockaddr_in6* addr )
{
	char buf[INET6_ADDRSTRLEN];
	if( inet_ntop( AF_INET6, &addr->sin6_addr, buf, sizeof( buf ) ) == NULL )
	{
		PyErr_SetFromErrno( PyExc_OSError );
		return NULL;
	}
	return PyUnicode_FromString( buf );
}
#endif

void UdpRecvRequest::onCallback( ICallbackParams* callbackParams )
{
	if ( m_timedOut )
	{
		return;
	}
	Ccp::PyGilEnsure gil;
	ON_BLOCK_EXIT( [&] { clearTimeout(); } );
	auto params = static_cast<UdpRecvRequest::Params*>(callbackParams);
	ssize_t nread = params->nread;
	const uv_buf_t* buf = params->buf;
	const struct sockaddr* addr = params->addr;
	unsigned flags = params->flags;

	auto bufferGuard = MakeGuard([&] {delete buf;});
	if ( nread < 0 )
	{
		PyErr_FromUvErr( int( nread ) );
		sendError("UdpRecvRequest::onRead failed to read data.");
		return;
	}

	s_bytesReceived += nread;

	if (!m_buf) {
		m_buf = PyBytes_FromStringAndSize( buf->base, nread );
	} else {
		PyBytes_ConcatAndDel( &m_buf, PyBytes_FromStringAndSize( buf->base, nread ) );
	}
	if ( !m_buf ) {
		sendError("UdpRecvRequest::onRead failed to create buffer");
		return;
	}

	if (addr && !m_addr)
	{
		switch( addr->sa_family )
		{
		case AF_INET: {
			const struct sockaddr_in* a = (const struct sockaddr_in*)addr;
			PyObject* addrobj = make_ipv4_addr( a );
			if( addrobj )
			{
				m_addr = Py_BuildValue( "Oi", addrobj, ntohs( a->sin_port ) );
				Py_DECREF( addrobj );
			}
		}
#ifdef ENABLE_IPV6
		case AF_INET6: {
			const struct sockaddr_in6* a = (const struct sockaddr_in6*)addr;
			PyObject* addrobj = make_ipv6_addr( a );
			if( addrobj )
			{
				m_addr = Py_BuildValue( "OiII",
										addrobj,
										ntohs( a->sin6_port ),
										ntohl( a->sin6_flowinfo ),
										a->sin6_scope_id );
				Py_DECREF( addrobj );
			}
		}
#endif /* ENABLE_IPV6 */
		}
	}

	if ( !m_addr ) {
		Py_DECREF( m_buf );
		sendError("UdpRecvRequest::onRead failed to create addr");
		return;
	}

	// no more data, let's signal that we're done
	if (nread == 0) {
		if ( g_scheduler->PyChannel_Send( m_channel, Py_None ) < 0 )
		{
			LogError( "UdpRecvRequest::onRead failed sending sentinel value on channel" );
			PyErr_Clear();
			return;
		}
	}

	if ( ! ( flags & UV_UDP_MMSG_CHUNK ) ) {
		bufferGuard.Dismiss();
	}
}

void UdpRecvRequest::onTimeout()
{
	uv_udp_recv_stop(handle());
	IRequest::onTimeout();
}

void UdpRecvRequest::cancel()
{
	uv_udp_recv_stop( handle() );
	// Check the balance, as this could be called
	// after the request has finished executing.
	if( g_scheduler->PyChannel_GetBalance( m_channel ) < 0 )
	{
		if( g_scheduler->PyChannel_Send( m_channel, Py_None ) < 0 )
		{
			LogError( "UdpRecvRequest::cancel failed sending sentinel value on channel" );
		}
	}
	IRequest::cancel();
}

PyObject* UdpSendRequest::execute()
{
	acquireSend("UdpSendRequest");
	ON_BLOCK_EXIT([&]{releaseSend("UdpSendRequest");});

	auto req = new write_req_t;

	int status = uv_udp_send( &m_writeRequest, handle(), &m_sendBuffer, 1, &m_addr, nullptr );
	if( status < 0 )
	{
		return PyLong_FromLong( status );
	}
	s_bytesSent += m_sendBuffer.len;
	auto ret = PyLong_FromSsize_t(m_sendBuffer.len);
	return ret;
}

void UdpSendRequest::sendCallback( uv_udp_send_t* request, int status )
{
	auto* data = reinterpret_cast<HandleData*>( request->handle->data );
	if( data->request )
	{
		auto _this = std::reinterpret_pointer_cast<UdpSendRequest>( data->request );
		auto params = std::make_unique<UdpSendRequest::Params>( status );
		_this->onCallback( params.get() );
	}
}

void UdpSendRequest::onCallback( ICallbackParams* callbackParams )
{
	if( m_timedOut ) // If we have timed out, the execute method has already been unblocked.
	{
		return;
	}
	auto *params = dynamic_cast<UdpSendRequest::Params*>(callbackParams);
	auto py_status = PyLong_FromLong( params->status );
	if( !py_status )
	{
		sendError( "UdpSendRequest::send Failed to convert status to python int" );
		return;
	}
	if( g_scheduler->PyChannel_Send( m_channel, py_status ) < 0 )
	{
		LogError( "UdpSendRequest::send Failed to send status over channel" );
		PyErr_Clear();
	}
}

static PyObject *
	makesockaddr(SOCKET_T sockfd, struct sockaddr *addr, size_t addrlen, int proto)
{
	if (addrlen == 0) {
		/* No address -- may be recvfrom() from known socket */
		Py_RETURN_NONE;
	}

	switch (addr->sa_family) {

	case AF_INET:
	{
		const struct sockaddr_in *a = (const struct sockaddr_in *)addr;
		PyObject *addrobj = make_ipv4_addr(a);
		PyObject *ret = NULL;
		if (addrobj) {
			ret = Py_BuildValue("Oi", addrobj, ntohs(a->sin_port));
			Py_DECREF(addrobj);
		}
		return ret;
	}

#if defined(AF_UNIX)
	case AF_UNIX:
	{
		struct sockaddr_un *a = (struct sockaddr_un *) addr;
#ifdef __linux__
		size_t linuxaddrlen = addrlen - offsetof(struct sockaddr_un, sun_path);
		if (linuxaddrlen > 0 && a->sun_path[0] == 0) {  /* Linux abstract namespace */
			return PyBytes_FromStringAndSize(a->sun_path, linuxaddrlen);
		}
		else
#endif /* linux */
		{
			/* regular NULL-terminated string */
			return PyUnicode_DecodeFSDefault(a->sun_path);
		}
	}
#endif /* AF_UNIX */

#if defined(AF_NETLINK)
	case AF_NETLINK:
	{
		struct sockaddr_nl *a = (struct sockaddr_nl *) addr;
		return Py_BuildValue("II", a->nl_pid, a->nl_groups);
	}
#endif /* AF_NETLINK */

#if defined(AF_QIPCRTR)
	case AF_QIPCRTR:
	{
		struct sockaddr_qrtr *a = (struct sockaddr_qrtr *) addr;
		return Py_BuildValue("II", a->sq_node, a->sq_port);
	}
#endif /* AF_QIPCRTR */

#if defined(AF_VSOCK)
	case AF_VSOCK:
	{
		struct sockaddr_vm *a = (struct sockaddr_vm *) addr;
		return Py_BuildValue("II", a->svm_cid, a->svm_port);
	}
#endif /* AF_VSOCK */

#ifdef ENABLE_IPV6
	case AF_INET6:
	{
		const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)addr;
		PyObject *addrobj = make_ipv6_addr(a);
		PyObject *ret = NULL;
		if (addrobj) {
			ret = Py_BuildValue("OiII",
								 addrobj,
								 ntohs(a->sin6_port),
								 ntohl(a->sin6_flowinfo),
								 a->sin6_scope_id);
			Py_DECREF(addrobj);
		}
		return ret;
	}
#endif /* ENABLE_IPV6 */

#ifdef USE_BLUETOOTH
	case AF_BLUETOOTH:
		switch (proto) {

#ifdef BTPROTO_L2CAP
		case BTPROTO_L2CAP:
		{
			struct sockaddr_l2 *a = (struct sockaddr_l2 *) addr;
			PyObject *addrobj = makebdaddr(&_BT_L2_MEMB(a, bdaddr));
			PyObject *ret = NULL;
			if (addrobj) {
				ret = Py_BuildValue("Oi",
									 addrobj,
									 _BT_L2_MEMB(a, psm));
				Py_DECREF(addrobj);
			}
			return ret;
		}

#endif /* BTPROTO_L2CAP */

		case BTPROTO_RFCOMM:
		{
			struct sockaddr_rc *a = (struct sockaddr_rc *) addr;
			PyObject *addrobj = makebdaddr(&_BT_RC_MEMB(a, bdaddr));
			PyObject *ret = NULL;
			if (addrobj) {
				ret = Py_BuildValue("Oi",
									 addrobj,
									 _BT_RC_MEMB(a, channel));
				Py_DECREF(addrobj);
			}
			return ret;
		}

#ifdef BTPROTO_HCI
		case BTPROTO_HCI:
		{
			struct sockaddr_hci *a = (struct sockaddr_hci *) addr;
#if defined(__NetBSD__) || defined(__DragonFly__)
			return makebdaddr(&_BT_HCI_MEMB(a, bdaddr));
#else /* __NetBSD__ || __DragonFly__ */
			PyObject *ret = NULL;
			ret = Py_BuildValue("i", _BT_HCI_MEMB(a, dev));
			return ret;
#endif /* !(__NetBSD__ || __DragonFly__) */
		}

#if !defined(__FreeBSD__)
		case BTPROTO_SCO:
		{
			struct sockaddr_sco *a = (struct sockaddr_sco *) addr;
			return makebdaddr(&_BT_SCO_MEMB(a, bdaddr));
		}
#endif /* !__FreeBSD__ */
#endif /* BTPROTO_HCI */

		default:
			PyErr_SetString(PyExc_ValueError,
							 "Unknown Bluetooth protocol");
			return NULL;
		}
#endif /* USE_BLUETOOTH */

#if defined(HAVE_NETPACKET_PACKET_H) && defined(SIOCGIFNAME)
	case AF_PACKET:
	{
		struct sockaddr_ll *a = (struct sockaddr_ll *)addr;
		const char *ifname = "";
		struct ifreq ifr;
		/* need to look up interface name give index */
		if (a->sll_ifindex) {
			ifr.ifr_ifindex = a->sll_ifindex;
			if (ioctl(sockfd, SIOCGIFNAME, &ifr) == 0)
				ifname = ifr.ifr_name;
		}
		return Py_BuildValue("shbhy#",
							  ifname,
							  ntohs(a->sll_protocol),
							  a->sll_pkttype,
							  a->sll_hatype,
							  a->sll_addr,
							  (Py_ssize_t)a->sll_halen);
	}
#endif /* HAVE_NETPACKET_PACKET_H && SIOCGIFNAME */

#ifdef HAVE_LINUX_TIPC_H
	case AF_TIPC:
	{
		struct sockaddr_tipc *a = (struct sockaddr_tipc *) addr;
		if (a->addrtype == TIPC_ADDR_NAMESEQ) {
			return Py_BuildValue("IIIII",
								  a->addrtype,
								  a->addr.nameseq.type,
								  a->addr.nameseq.lower,
								  a->addr.nameseq.upper,
								  a->scope);
		} else if (a->addrtype == TIPC_ADDR_NAME) {
			return Py_BuildValue("IIIII",
								  a->addrtype,
								  a->addr.name.name.type,
								  a->addr.name.name.instance,
								  a->addr.name.name.instance,
								  a->scope);
		} else if (a->addrtype == TIPC_ADDR_ID) {
			return Py_BuildValue("IIIII",
								  a->addrtype,
								  a->addr.id.node,
								  a->addr.id.ref,
								  0,
								  a->scope);
		} else {
			PyErr_SetString(PyExc_ValueError,
							 "Invalid address type");
			return NULL;
		}
	}
#endif /* HAVE_LINUX_TIPC_H */

#if defined(AF_CAN) && defined(SIOCGIFNAME)
	case AF_CAN:
	{
		struct sockaddr_can *a = (struct sockaddr_can *)addr;
		const char *ifname = "";
		struct ifreq ifr;
		/* need to look up interface name given index */
		if (a->can_ifindex) {
			ifr.ifr_ifindex = a->can_ifindex;
			if (ioctl(sockfd, SIOCGIFNAME, &ifr) == 0)
				ifname = ifr.ifr_name;
		}

		switch (proto) {
#ifdef CAN_ISOTP
		case CAN_ISOTP:
		{
			return Py_BuildValue("O&kk", PyUnicode_DecodeFSDefault,
								  ifname,
								  a->can_addr.tp.rx_id,
								  a->can_addr.tp.tx_id);
		}
#endif /* CAN_ISOTP */
#ifdef CAN_J1939
		case CAN_J1939:
		{
			return Py_BuildValue("O&KIB", PyUnicode_DecodeFSDefault,
								  ifname,
								  (unsigned long long)a->can_addr.j1939.name,
								  (unsigned int)a->can_addr.j1939.pgn,
								  a->can_addr.j1939.addr);
		}
#endif /* CAN_J1939 */
		default:
		{
			return Py_BuildValue("(O&)", PyUnicode_DecodeFSDefault,
								  ifname);
		}
		}
	}
#endif /* AF_CAN && SIOCGIFNAME */

#ifdef PF_SYSTEM
	case PF_SYSTEM:
		switch(proto) {
#ifdef SYSPROTO_CONTROL
		case SYSPROTO_CONTROL:
		{
			struct sockaddr_ctl *a = (struct sockaddr_ctl *)addr;
			return Py_BuildValue("(II)", a->sc_id, a->sc_unit);
		}
#endif /* SYSPROTO_CONTROL */
		default:
			PyErr_SetString(PyExc_ValueError,
							 "Invalid address type");
			return 0;
		}
#endif /* PF_SYSTEM */

#ifdef HAVE_SOCKADDR_ALG
	case AF_ALG:
	{
		struct sockaddr_alg *a = (struct sockaddr_alg *)addr;
		return Py_BuildValue("s#s#HH",
							  a->salg_type,
							  strnlen((const char*)a->salg_type,
									   sizeof(a->salg_type)),
							  a->salg_name,
							  strnlen((const char*)a->salg_name,
									   sizeof(a->salg_name)),
							  a->salg_feat,
							  a->salg_mask);
	}
#endif /* HAVE_SOCKADDR_ALG */

#ifdef HAVE_AF_HYPERV
	case AF_HYPERV:
	{
		SOCKADDR_HV *a = (SOCKADDR_HV *) addr;

		wchar_t *guidStr;
		RPC_STATUS res = UuidToStringW(&a->VmId, (RPC_WSTR *) &guidStr);
		if (res != RPC_S_OK) {
			PyErr_SetFromWindowsErr(res);
			return 0;
		}
		PyObject *vmId = PyUnicode_FromWideChar(guidStr, -1);
		res = RpcStringFreeW((RPC_WSTR *)&guidStr);
		assert(res == RPC_S_OK);

		res = UuidToStringW(&a->ServiceId, (RPC_WSTR *) &guidStr);
		if (res != RPC_S_OK) {
			Py_DECREF(vmId);
			PyErr_SetFromWindowsErr(res);
			return 0;
		}
		PyObject *serviceId = PyUnicode_FromWideChar(guidStr, -1);
		res = RpcStringFreeW((RPC_WSTR *)&guidStr);
		assert(res == RPC_S_OK);

		return Py_BuildValue("NN", vmId, serviceId);
	}
#endif /* AF_HYPERV */

		/* More cases here... */

	default:
		/* If we don't know the address family, don't raise an
           exception -- return it as an (int, bytes) tuple. */
		return Py_BuildValue("iy#",
							  addr->sa_family,
							  addr->sa_data,
							  sizeof(addr->sa_data));

	}
}

PyObject* StreamAcceptRequest::execute()
{
	associateWithHandleData();
	ON_BLOCK_EXIT( [this] { clearTimeout(); } );

	auto result = startTimeout();
	if( result != Py_None )
	{
		return nullptr;
	}

	PyObject* listen_status{ nullptr};
	if ( handleData()->pendingAccepts.empty() ) {
		listen_status = g_scheduler->PyChannel_Receive( m_channel );
	} else {
		listen_status = handleData()->pendingAccepts.front();
		handleData()->pendingAccepts.pop_front();
	}

	if( !listen_status )
	{
		return nullptr;
	}
	ON_BLOCK_EXIT( [&] { Py_XDECREF( listen_status ); } );
	if( !PyLong_Check( listen_status ) )
	{
		PyErr_BadInternalCall();
		return nullptr;
	}

	auto status = PyLong_AsLong( listen_status );
	if( status < 0 )
	{
		if( !PyErr_Occurred() )
		{
			PyErr_FromUvErr( status );
		}
		return nullptr;
	}

	auto *client = new uv_tcp_t;
	uv_tcp_init(handle()->loop, client);
	status = uv_accept(handle(), reinterpret_cast<uv_stream_t*>(client));
	auto guard = MakeGuard([&] {uv_close((uv_handle_t*)client, cleanup_uv_handle);});
	if( status < 0 ) {
		PyErr_FromUvErr(status);
		return nullptr;
	}
	client->close_cb = nullptr;
	SOCKET_T newfd;
	status = uv_fileno( reinterpret_cast<const uv_handle_t*>( client ), reinterpret_cast<uv_os_fd_t*>( &newfd ) );
	if( status < 0 )
	{
		PyErr_FromUvErr( status );
		return nullptr;
	}
	client->data = CreateHandleData();
	if( !client->data )
	{
		return nullptr;
	}
	AddToLookupTable( newfd, reinterpret_cast<uv_handle_t*>( client ) );

	auto py_fd = PyLong_FromSocket_t( newfd );
	if( !py_fd )
	{
		return nullptr;
	}
	ON_BLOCK_EXIT([&] { Py_DecRef(py_fd); } );

	// Get the peer name
	sock_addr_t addrbuf;
	int addrlen = sizeof( struct sockaddr_in6 );
	memset(&addrbuf, 0, addrlen);

	status = uv_tcp_getpeername( reinterpret_cast<uv_tcp_t*>( client ), &addrbuf.sa , &addrlen );
	if( status < 0 )
	{
		PyErr_FromUvErr( status );
		return nullptr;
	}
	auto py_address = makesockaddr(newfd, &addrbuf.sa, addrlen, 0);
	if( !py_address )
	{
		return nullptr;
	}
	ON_BLOCK_EXIT( [&] { Py_DecRef(py_address); } );

	auto tuple = PyTuple_Pack(2, py_fd, py_address );
	if( !tuple )
	{
		return nullptr;
	}

	guard.Dismiss();
	return tuple;

}

StreamRecvIntoRequest::StreamRecvIntoRequest( PySocketSockObject* s, char* buf, Py_ssize_t length, int flags ) :
	StreamRecvRequest( s, length, flags ), m_buf(buf)
{

}
int StreamRecvIntoRequest::startRead()
{
	return uv_read_start( handle(), StreamRecvIntoRequest::alloc, StreamRecvRequest::readCallback );
}

void StreamRecvIntoRequest::alloc( uv_handle_t* handle, size_t size, uv_buf_t* buf )
{
	auto* data = reinterpret_cast<HandleData*>(handle->data);
	auto request = std::reinterpret_pointer_cast<StreamRecvIntoRequest>(data->receiveRequest);

	buf->base = request->m_buf;
	buf->len = ULONG( request->m_requested_len );

	ssize_t unreadBytes = data->bufWritePos - data->bufReadPos;

	// StreamRecvRequest's receive function should ensure that uv_read_start
	// doesn't get called when we already have all the data on hand.
	assert(unreadBytes < request->m_requested_len);

	if( unreadBytes > 0 )
	{
		auto copyAmount = unreadBytes < request->m_requested_len ? unreadBytes : request->m_requested_len;
		memcpy_s(buf->base, copyAmount, data->buf.base + data->bufReadPos, copyAmount);
		buf->base += copyAmount;
		buf->len -= ULONG( copyAmount );
		data->bufReadPos += copyAmount;
	}
}

PyObject* StreamRecvIntoRequest::constructResult( HandleData* data ) const
{
	ssize_t bufferedAmount = data->bufWritePos - data->bufReadPos;
	auto chunkSize = bufferedAmount < m_requested_len ? bufferedAmount : m_requested_len;
	data->bufReadPos += chunkSize;
	return PyLong_FromSsize_t(chunkSize);
}

StreamConnectRequest::StreamConnectRequest( PySocketSockObject* socket, struct sockaddr* address ) :
	IStreamRequest( socket ), m_address( address )
{
	m_connect = new uv_connect_t;
}
StreamConnectRequest::~StreamConnectRequest()
{
	delete m_connect;
}

void StreamConnectRequest::onTimeout()
{
	m_connect->data = nullptr;
	IRequest::onTimeout();
}

PyObject* StreamConnectRequest::execute()
{
	auto ret = startTimeout();
	if( !ret )
	{
		return nullptr;
	}
	Py_DecRef(ret);
	handleData()->request = shared_from_this();
	m_connect->data = this;
	int status = uv_tcp_connect(m_connect, reinterpret_cast<uv_tcp_t*>( handle() ), m_address, &StreamConnectRequest::connectCallback);
	if ( status < 0 )
	{
		PyErr_FromUvErr( status );
		return nullptr;
	}
	PyObject* connect_status = g_scheduler->PyChannel_Receive(m_channel);
	if( connect_status == nullptr ) {
		return nullptr;
	}
	if( !PyLong_Check( connect_status ) ) {
		PyErr_BadInternalCall();
		return nullptr;
	}
	status = PyLong_AsLong( connect_status );
	if( status < 0 ) {
		if( !PyErr_Occurred() )
		{
			PyErr_FromUvErr( status );
		}
		return nullptr;
	}

	Py_RETURN_NONE;
}

void StreamConnectRequest::connectCallback( uv_connect_t* connection, int status )
{
	if ( connection->data )
	{
		auto _this = static_cast<StreamConnectRequest*>( connection->data );
		auto params = std::make_unique<StreamConnectRequest::Params>( status );
		_this->onCallback( params.get() );
	}
}

void StreamConnectRequest::onCallback( ICallbackParams* callbackParams )
{
	if( m_timedOut ) // If we have timed out, the execute method has already been unblocked.
	{
		return;
	}
	ON_BLOCK_EXIT( [this] { clearTimeout(); } );
	Ccp::PyGilEnsure gil;
	auto *params = dynamic_cast<StreamConnectRequest::Params*>(callbackParams);

	auto py_status = PyLong_FromLong( params->status );
	if( py_status == nullptr )
	{
		PyObject *exc, *val, *tb;
		PyErr_Fetch( &exc, &val, &tb );
		auto ret = g_scheduler->PyChannel_SendThrow( m_channel, exc, val, tb );
		if( ret < 0 )
		{
			PyErr_Restore( exc, val, tb );
			LogError( "StreamConnectRequest::onConnect failed to send exception" );
			PyErr_Clear();
		}
		return;
	}
	int ret = g_scheduler->PyChannel_Send( m_channel, py_status );
	if( ret < 0 )
	{
		LogError( "StreamConnectRequest::onConnect failed to send status" );
		PyErr_Clear();
	}
}

extern "C" int FormatPacket( char* buf, const char* data, const uint32_t dataLen, const char* OOBData, const uint32_t OOBLen );

PyObject* SendPacket( PySocketSockObject* socket, void* data, Py_ssize_t len )
{
	Py_ssize_t bufsize = len + sizeof(uint32_t);
	std::vector<char> buf(bufsize);
	size_t outlen = FormatPacket( buf.data(), static_cast<const char*>( data ), len, nullptr, 0 );
	if ( outlen == 0 )
	{
		PyErr_SetString( PyExc_MemoryError, "Failed formatting packet data" );
		return nullptr;
	}

	auto handleData = reinterpret_cast<HandleData*>(socket->uv_handle->data);
	auto req = std::make_shared<StreamSendRequest>( socket, buf.data(), outlen, 0, handleData->blockingSend );
	s_packetsSent += 1;

	return req->execute();
}

extern "C" void AddOobDataCallback( OobDataCallback packetCallback )
{
	s_oobDataCallbacks.push_back( packetCallback );
}

extern "C" void RemoveOobDataCallback( OobDataCallback packetCallback )
{
	s_oobDataCallbacks.erase( std::remove( s_oobDataCallbacks.begin(), s_oobDataCallbacks.end(), packetCallback ), s_oobDataCallbacks.end() );
}

// Below methods are used by BlueNet and therefore avoid usage of Python

void SendFormattedPacketWriteCallback(uv_write_t* request, int status)
{
	if (status < 0)
	{
		CCP_LOGERR( "Failed writing data: %s", uv_err_name( status ) );
	}

	auto* bufs = static_cast<uv_buf_t*>( request->data );
	delete[] bufs[0].base;
	delete[] bufs; // clean up the buffer array
	delete request; // clean up the request
}

extern "C" int SendFormattedPacket( long long fd, const char* data, unsigned int len )
{
	// Assumes the packet is already formatted according to FormatPacket
	auto uv_handle = LookupHandle( fd );
	if ( !uv_handle )
	{
		CCP_LOGERR( "Cannot send data for socket %lld because there's no matching libuv handle", fd );
		return 0;
	}

	if ( uv_handle_get_type( uv_handle ) != UV_TCP )
	{
		CCP_LOGERR( "BlueNet only supports TCP sockets" );
		return 0;
	}

	auto* bufs = new uv_buf_t[1];
	bufs[0] = uv_buf_init( new char[len], len );
	memcpy( bufs[0].base, data, len );
	auto* write_req = new uv_write_t;
	write_req->data = bufs;
	int status = uv_write( write_req, reinterpret_cast<uv_stream_t*>( uv_handle ), bufs, 1, SendFormattedPacketWriteCallback );

	if ( status != 0 )
	{
		CCP_LOGERR( "libuv failed writing packet: %s", uv_err_name( status ) );
		return 0;
	}

	return 1;
}

extern "C" int SendPacket( long long fd, const char* data, unsigned int len, const char* OOBData, unsigned int OOBLen )
{
	size_t bufsize = sizeof(uint32_t) * 2 + len + OOBLen;
	auto buf = new char[bufsize];
	ON_BLOCK_EXIT([buf] {delete[] buf;});
	size_t outlen = FormatPacket( buf, data, len, OOBData, OOBLen );
	if ( outlen == 0 )
	{
		return 0;
	}
	// SendFormattedPacket takes ownership of `buf` at this point
	return SendFormattedPacket( fd, buf, outlen );
}

extern "C" int FormatPacket( char* buf, const char* data, const uint32_t dataLen, const char* OOBData, const uint32_t OOBLen )
{
	if ( !buf || !data )
	{
		return 0;
	}

	size_t pos;
	if ( OOBData && OOBLen )
	{
		*(uint32_t *)buf = htonl( dataLen + OOBLen + sizeof(uint32_t)) | htonl( ceHeaderExpectPayloadOffset );
		*(uint32_t *)(buf + sizeof(uint32_t )) = htonl( OOBLen );
		memcpy( buf + sizeof(uint32_t) * 2, OOBData, OOBLen );
		pos = OOBLen + sizeof(uint32_t) * 2;
	}
	else
	{
		*(uint32_t *)buf = htonl( dataLen );
		pos = sizeof(uint32_t);
	}

	memcpy( buf + pos, data, dataLen );
	return dataLen + pos;
}

void AugmentSocketAPI( PySocketModule_APIObject* apiObject )
{
	apiObject->dispatch = TickUvLoop;
	apiObject->add_oob_data_callback = AddOobDataCallback;
	apiObject->remove_oob_data_callback = RemoveOobDataCallback;
	apiObject->format_packet = FormatPacket;
	apiObject->send_formatted_packet = SendFormattedPacket;
	apiObject->send_packet = SendPacket;
}

bool StreamPacketReceiveRequest::readHeader( char* src )
{
	// Only read the header if we haven't read it already
	if (m_packetHeader != 0)
	{
		return true;
	}

	m_packetHeader = ntohl( *reinterpret_cast<uint32_t*>( src ) );
	handleData()->bufReadPos += sizeof( m_packetHeader );
	if ( payloadLen() > handleData()->maxPacketSize )
	{
		CCP_LOGERR( "Handle %p readHeader for %p: too large a packet detected (%d bytes)", m_handle, handleData()->request.get(), m_packetHeader );
		PyErr_Format( PyExc_OSError, "too large a packet detected at %d bytes, max is %llu", payloadLen(), handleData()->maxPacketSize );
		return false;
	}

	m_data.resize( payloadLen() );

	return true;
}

bool StreamPacketReceiveRequest::needMore() {
	auto data = handleData();
	auto bytesRemaining = data->bufWritePos - data->bufReadPos;
	if ( !m_packetHeader && bytesRemaining >= sizeof( uint32_t ) )
	{
		if( !readHeader( data->buf.base + data->bufReadPos ) )
		{
			return false;
		}
		bytesRemaining -= sizeof( m_packetHeader );
	}

	if ( bytesRemaining > 0 ) {
		// do we have even more bytes remaining that we can already fill into the buffer?
		auto spaceLeftInBuffer = m_data.size() - m_bytesRead;
		auto copyAmount = bytesRemaining >= spaceLeftInBuffer ? spaceLeftInBuffer : bytesRemaining;
		if ( copyAmount > 0 ) {
			auto bufferStart = copyAmount == data->buf.len ? data->buf.base : data->buf.base + data->bufReadPos;
			memcpy_s( m_data.data() + m_bytesRead, m_data.size() - m_bytesRead, bufferStart, copyAmount );
			data->bufReadPos += copyAmount;
			m_bytesRead += copyAmount;
		}
	}

	if ( m_bytesRead == m_data.size() )
	{
		m_payload = m_data.data();
		m_payloadEnd = m_payload + payloadLen();

		if ( (m_packetHeader & ceHeaderExpectPayloadOffset) == ceHeaderExpectPayloadOffset)
		{
			m_oobDataLen = ntohl( *reinterpret_cast<decltype( m_oobDataLen )*>( m_payload ) );

			if ( m_oobDataLen > data->maxPacketSize )
			{
				PyErr_Format(PyExc_OSError, "corrupted out-of-band data in packet detected at %d bytes, max is %llu", m_oobDataLen, data->maxPacketSize);
				return false;
			}

			m_payload += sizeof(m_oobDataLen);
			m_oobData = m_payload;
			m_payload += m_oobDataLen;
			for ( auto callback : s_oobDataCallbacks ) {
				auto stop = callback(
					static_cast<long long>( m_fd ),
					m_oobData + m_oobDataLen,
					payloadLen() - m_oobDataLen,
					m_oobData,
					m_oobDataLen
				);
				if (stop != 0) {
					// BlueNet ate the packet, so reset our internal state
					m_data.clear();
					m_packetHeader = 0;
					m_bytesRead = 0;
					return false;
				}
			}
		}
	}

	return m_data.empty() || m_bytesRead < m_data.size();
};

PyObject* StreamPacketReceiveRequest::execute()
{
	acquireReceive("StreamPacketReceiveRequest");
	ON_BLOCK_EXIT([&]{releaseReceive("StreamPacketReceiveRequest");});
	auto* data = handleData();

	auto sequenceNumber = data->packetNumber++;

	auto ret = startTimeout();
	if( ret != Py_None )
	{
		return nullptr;
	}
	ON_BLOCK_EXIT( [&] { clearTimeout(); } );

	while ( !m_timedOut && needMore() )
	{
		auto status = startRead();
		if ( status == 0 )
		{
			auto sentinel = g_scheduler->PyChannel_Receive( m_channel );
			if( !sentinel )
			{
				return nullptr;
			}
		} else {
			PyErr_FromUvErr( status );
			return nullptr;
		}
	}

	if ( PyErr_Occurred() ) {
		return nullptr;
	}

	s_packetsReceived += 1;

	Py_IncRef( Py_None );
	auto packetSize = m_payloadEnd - m_payload;

	auto* packet = PyTuple_Pack( 3, PyBytes_FromStringAndSize( m_payload, packetSize ), PyBytes_FromStringAndSize( m_oobData, m_oobDataLen ), PyLong_FromSize_t( sequenceNumber ) );
	if ( !packet ) {
		Py_DecRef( Py_None );
	}

	return packet;
}

int StreamPacketReceiveRequest::startRead()
{
	return uv_read_start( handle(), growingBufferAlloc, StreamPacketReceiveRequest::readCallback );
}

void StreamPacketReceiveRequest::readCallback( uv_stream_t* client, ssize_t nread, const uv_buf_t* buf )
{
	auto* data = reinterpret_cast<HandleData*>( client->data );
	if( data->receiveRequest )
	{
		auto _this = std::reinterpret_pointer_cast<StreamPacketReceiveRequest>( data->receiveRequest );
		auto params = Params( nread, buf );
		_this->onCallback( &params );
	}
}

size_t StreamPacketReceiveRequest::payloadLen() const
{
	return m_packetHeader & ceHeaderSizeMask;
}

void StreamPacketReceiveRequest::stopRead()
{
	uv_read_stop( handle() );
}

void StreamPacketReceiveRequest::onCallback( ICallbackParams* callbackParams )
{
	auto params = dynamic_cast<Params*>(callbackParams);
	ssize_t nread = params->nread;
	Ccp::PyGilEnsure gil;
	g_scheduler->PyChannel_SetPreference(m_channel, PREFER_SENDER );
	if ( nread < 0 ) {
		if (nread != UV_EOF) {
			stopRead();
			PyErr_FromUvErr( int( nread ) );
			sendError("OnReceive failed to read data.");
		}
		else {
			stopRead();
			// Nothing left to read
			if ( g_scheduler->PyChannel_Send( m_channel, Py_None ) < 0 ) {
				LogError( "StreamRecvRequest::onReceive failed to signal sentinel" );
				PyErr_Clear();
			}
		}
	}
	if ( nread >= 0 ) {
		s_bytesReceived += nread;
		handleData()->bufWritePos += nread;
		stopRead();
		if ( g_scheduler->PyChannel_Send( m_channel, Py_None ) < 0 ) {
			LogError( "StreamRecvRequest::onReceive failed to signal sentinel" );
			PyErr_Clear();
		}
	}
}
