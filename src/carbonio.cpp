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
		if( data->receiveRequest )
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

bool is_valid_socket( PySocketSockObject* socket )
{
	return socket->sock_fd != INVALID_SOCKET && is_valid_uv_handle( socket->uv_handle );
}

HandleData::HandleData() : channel( PyChannel_New( nullptr ) ), packetReceiveQueue( PyChannel_New( nullptr ) ), request( nullptr )
{
	buf = uv_buf_init( nullptr, 0 );
}

HandleData::~HandleData()
{
	Py_XDECREF( channel );
	channel = nullptr;
	Py_XDECREF( packetReceiveQueue );
	packetReceiveQueue = nullptr;
	delete buf.base;
	buf.base = nullptr;
	buf.len = 0;
	bufReadPos = -1;
	bufWritePos = -1;
}


void* CreateHandleData()
{
	auto* data = new HandleData;
	if( data->channel == nullptr )
	{
		Py_XDECREF( data->packetReceiveQueue );
		delete data;
		return nullptr;
	}
	if( data->packetReceiveQueue == nullptr )
	{
		Py_DECREF( data->channel );
		delete data;
		return nullptr;
	}

	PyChannel_SetPreference( data->packetReceiveQueue, PREFER_SENDER );
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

IRequest::IRequest( PySocketSockObject* socket ) : m_handle( socket->uv_handle ), m_timeout_nanoseconds(socket->sock_timeout)
{
	handleData()->request.reset(this);
	m_self = handleData()->request;
	m_channel = PyChannel_New( nullptr );
	if( !m_channel )
	{
		LogError( "Failed to create channel for request" );
	}
	else
	{
		PyChannel_SetPreference( m_channel, PREFER_SENDER );
	}
}

void IRequest::sendError(std::string_view msg)
{
	PyObject *exc, *val, *tb;
	PyErr_Fetch( &exc, &val, &tb );
	PyChannel_SetPreference(m_channel, PREFER_SENDER );
	auto ret = PyChannel_SendThrow( m_channel, exc, val, tb);
	if( ret < 0 )
	{
		PyErr_Restore( exc, val, tb );
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
		auto sentinel = PyChannel_Receive( m_channel );
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
	auto params = dynamic_cast<StreamRecvRequest::Params*>(callbackParams);
	ssize_t nread = params->nread;
	if( nread == 0 ) {
		return;
	}
	Ccp::PyGilEnsure gil;
	ON_BLOCK_EXIT( [&] { clearTimeout(); finalize();} );
	PyChannel_SetPreference(m_channel, PREFER_SENDER );
	if ( nread < 0 ) {
		if (nread != UV_EOF) {
			PyErr_FromUvErr( int( nread ) );
			sendError("OnReceive failed to read data.");
		}
		else {
			if ( PyChannel_Send( m_channel, Py_None ) < 0 ) {
				LogError( "StreamRecvRequest::onReceive failed to signal sentinel" );
				PyErr_Clear();
			}
		}
	}
	if ( nread > 0 ) {
		m_received_len += nread;
		uv_read_stop( handle() );
		if ( PyChannel_Send( m_channel, Py_None ) < 0 ) {
			LogError( "StreamRecvRequest::onReceive failed to signal sentinel" );
			PyErr_Clear();
		}
	}
}

void StreamRecvRequest::alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf)
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
	if( data->receiveRequest )
	{
		auto _this = reinterpret_cast<StreamRecvRequest*>( data->receiveRequest.get() );
		auto params = StreamRecvRequest::Params( nread, buf );
		_this->onCallback( &params );
	}
}

void StreamRecvRequest::cancel()
{
	uv_read_stop( handle() );
	// Check the balance, as this could be called after the request has finished executing.
	if( PyChannel_GetBalance( m_channel ) < 0 )
	{
		if( PyChannel_Send( m_channel, Py_None ) < 0 )
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
	handleData()->receiveRequest = m_self;
	return uv_read_start( handle(), StreamRecvRequest::alloc, StreamRecvRequest::readCallback );
}

PyObject* StreamSendRequest::execute()
{
	if ( ! startTimeout() )
	{
		return nullptr;
	}
	auto currentTasklet = reinterpret_cast<PyTaskletObject*>( PyStackless_GetCurrent() );
	if( m_blockingSend && PyTasklet_GetBlockTrap( currentTasklet ) )
	{
		PyErr_SetString(PyExc_RuntimeError, "Can't perform blocking send on a block trapped tasklet");
		return nullptr;
	}
	if( PyTasklet_IsMain( currentTasklet ) )
	{
		PyErr_SetString(PyExc_RuntimeError, "Can't perform blocking send on the main tasklet");
		return nullptr;
	}
	m_writeRequest.data = this;
	int status = uv_write(&m_writeRequest, handle(), &m_sendBuffer, 1, StreamSendRequest::sendCallback );
	if( status < 0 ){
		return PyLong_FromLong(status);
	}
	s_bytesSent += m_sendBuffer.len;

	if( m_blockingSend )
	{
		return PyChannel_Receive( m_channel );
	}
	return PyLong_FromLong(0);
}

void StreamSendRequest::sendCallback( uv_write_t* request, int status )
{
	auto *_this = reinterpret_cast<StreamSendRequest*>( request->data );
	auto params = std::make_unique<StreamSendRequest::Params>( status );
	_this->onCallback( params.get() );
}

void StreamSendRequest::onCallback( ICallbackParams* callbackParams )
{
	ON_BLOCK_EXIT( [this] { finalize(); } );
	if( m_timedOut ) // If we have timed out, the execute method has already been unblocked.
	{
		return;
	}
	ON_BLOCK_EXIT( [&] { clearTimeout(); } );
	auto *params = dynamic_cast<StreamSendRequest::Params*>(callbackParams);
	Ccp::PyGilEnsure gil;

	auto py_status = PyLong_FromLong(params->status);
	if( !py_status ){
		sendError("StreamSendRequest::send Failed to convert status to python int");
		return;
	}
	if( m_blockingSend && !m_timedOut )
	{
		if( PyChannel_Send( m_channel, py_status ) < 0 )
		{
			LogError( "StreamSendRequest::send Failed to send status over channel" );
			PyErr_Clear();
		}
	}
}

void SendError(PyChannelObject* channel, std::string_view msg)
{
	PyObject *exc, *val, *tb;
	PyErr_Fetch( &exc, &val, &tb );
	auto ret = PyChannel_SendThrow( channel, exc, val, tb);
	if( ret < 0 )
	{
		PyErr_Restore( exc, val, tb );
		LogError( msg.data() );
	}
}

PyObject* UdpRecvRequest::execute()
{
	auto ret = startTimeout();
	if( ret != Py_None )
	{
		return nullptr;
	}

	handleData()->receiveRequest = m_self;
	auto status = uv_udp_recv_start( handle(), alloc, UdpRecvRequest::receiveCallback );
	if ( status < 0 )
	{
		PyErr_FromUvErr( status );
		return nullptr;
	}

	auto sentinel = PyChannel_Receive( m_channel );
	if( !sentinel )
	{
		return nullptr;
	}

	return PyTuple_Pack( 2, m_buf, m_addr );
}

void UdpRecvRequest::receiveCallback( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags )
{
	auto* data = reinterpret_cast<HandleData*>( handle->data );
	if( data->receiveRequest )
	{
		auto _this = reinterpret_cast<UdpRecvRequest*>( data->receiveRequest.get() );
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
	auto params = static_cast<UdpRecvRequest::Params*>(callbackParams);
	ssize_t nread = params->nread;
	const uv_buf_t* buf = params->buf;
	const struct sockaddr* addr = params->addr;
	unsigned flags = params->flags;

	auto bufferGuard = MakeGuard([&] {delete buf;});
	auto requestGuard = MakeGuard([&] { finalize();});
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
		if ( PyChannel_Send( m_channel, Py_None ) < 0 )
		{
			LogError( "UdpRecvRequest::onRead failed sending sentinel value on channel" );
			PyErr_Clear();
			return;
		}
	}

	if ( ! ( flags & UV_UDP_MMSG_CHUNK ) ) {
		bufferGuard.Dismiss();
		requestGuard.Dismiss();
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
	if( PyChannel_GetBalance( m_channel ) < 0 )
	{
		if( PyChannel_Send( m_channel, Py_None ) < 0 )
		{
			LogError( "UdpRecvRequest::cancel failed sending sentinel value on channel" );
		}
	}
	IRequest::cancel();
}

PyObject* UdpSendRequest::execute()
{
	auto* request = new uv_udp_send_t;
	ON_BLOCK_EXIT( [&] { delete request; } );

	int status = uv_udp_send( &m_writeRequest, handle(), &m_sendBuffer, 1, m_addr, UdpSendRequest::sendCallback );
	if( status < 0 )
	{
		return PyLong_FromLong( status );
	}
	auto ret = PyChannel_Receive( m_channel );
	status = PyLong_AsLong( ret );
	if ( status < 0 ) {
		if( !( status == -1 && PyErr_Occurred() ) )
		{
			PyErr_FromUvErr( status );
		}
		return nullptr;
	}
	s_bytesSent += m_sendBuffer.len;
	ret = PyLong_FromSsize_t(m_sendBuffer.len);
	return ret;
}

void UdpSendRequest::sendCallback( uv_udp_send_t* request, int status )
{
	auto* data = reinterpret_cast<HandleData*>( request->handle->data );
	if( data->request )
	{
		auto _this = reinterpret_cast<UdpSendRequest*>( data->request.get() );
		auto params = std::make_unique<UdpSendRequest::Params>( status );
		_this->onCallback( params.get() );
	}
}

void UdpSendRequest::onCallback( ICallbackParams* callbackParams )
{
	ON_BLOCK_EXIT( [this] { finalize(); } );
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
	if( PyChannel_Send( m_channel, py_status ) < 0 )
	{
		LogError( "UdpSendRequest::send Failed to send status over channel" );
		PyErr_Clear();
	}
}

PyObject* StreamAcceptRequest::execute()
{
	ON_BLOCK_EXIT( [this] { clearTimeout(); finalize(); } );

	auto result = startTimeout();
	if( result != Py_None )
	{
		return nullptr;
	}

	result = PyChannel_Receive( m_channel );

	if( !result )
	{
		return nullptr;
	}
	ON_BLOCK_EXIT( [&] { Py_XDECREF( result ); } );
	auto listen_status = PyTuple_GetItem( result, 0 );

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
	return PyTuple_GetSlice(result, 1, 3);
}

StreamRecvIntoRequest::StreamRecvIntoRequest( PySocketSockObject* s, char* buf, Py_ssize_t length, int flags ) :
	StreamRecvRequest( s, length, flags ), m_buf(buf)
{

}
int StreamRecvIntoRequest::startRead()
{
	handleData()->receiveRequest = m_self;
	return uv_read_start( handle(), StreamRecvIntoRequest::alloc, StreamRecvRequest::readCallback );
}

void StreamRecvIntoRequest::alloc( uv_handle_t* handle, size_t size, uv_buf_t* buf )
{
	auto* data = reinterpret_cast<HandleData*>(handle->data);
	auto* request = reinterpret_cast<StreamRecvIntoRequest*>(data->request.get());

	buf->base = request->m_buf;
	buf->len = ULONG( request->m_requested_len );

	ssize_t unreadBytes = data->bufReadPos - data->bufWritePos;

	// StreamRecvRequest's receive function should ensure that uv_read_start
	// doesn't get called when we already have all the data on hand.
	assert(unreadBytes < request->m_requested_len);

	if( unreadBytes > 0 )
	{
		auto copyAmount = unreadBytes < request->m_requested_len ? unreadBytes : request->m_requested_len;
		memcpy_s(buf->base, copyAmount, data->buf.base + data->bufReadPos, copyAmount);
		buf->base += copyAmount;
		buf->len -= ULONG( copyAmount );
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

PyObject* StreamConnectRequest::execute()
{
	auto ret = startTimeout();
	if( !ret )
	{
		return nullptr;
	}
	Py_DecRef(ret);
	int status = uv_tcp_connect(m_connect, reinterpret_cast<uv_tcp_t*>( handle() ), m_address, &StreamConnectRequest::connectCallback);
	if ( status < 0 )
	{
		PyErr_FromUvErr( status );
		return nullptr;
	}
	PyObject* connect_status = PyChannel_Receive(m_channel);
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
	auto _this = reinterpret_cast<StreamConnectRequest*>(reinterpret_cast<HandleData*>(connection->handle->data)->request.get());
	auto params = std::make_unique<StreamConnectRequest::Params>( status );
	_this->onCallback( params.get() );
}

void StreamConnectRequest::onCallback( ICallbackParams* callbackParams )
{
	ON_BLOCK_EXIT( [this] { finalize(); } );
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
		auto ret = PyChannel_SendThrow( m_channel, exc, val, tb );
		if( ret < 0 )
		{
			PyErr_Restore( exc, val, tb );
			LogError( "StreamConnectRequest::onConnect failed to send exception" );
			PyErr_Clear();
		}
		return;
	}
	int ret = PyChannel_Send( m_channel, py_status );
	if( ret < 0 )
	{
		LogError( "StreamConnectRequest::onConnect failed to send status" );
		PyErr_Clear();
	}
}

extern "C" int FormatPacket( char* buf, const char* data, unsigned int dataLen, const char* OOBData, unsigned int OOBLen );

PyObject* SendPacket( PySocketSockObject* socket, void* data, Py_ssize_t len )
{
	Py_ssize_t bufsize = len + sizeof(uint32_t);
	char* buf = new char[bufsize];
	size_t outlen = FormatPacket( buf, static_cast<const char*>( data ), len, nullptr, 0 );
	if ( outlen == 0 )
	{
		PyErr_SetString( PyExc_MemoryError, "Failed formatting packet data" );
		return nullptr;
	}

	auto handleData = reinterpret_cast<HandleData*>(socket->uv_handle->data);
	auto req = new StreamSendRequest( socket, buf, outlen, 0, handleData->blockingSend );
	s_packetsSent += 1;
	return req->execute();
}

PyObject* ReceivePacket( PySocketSockObject* socket )
{
	auto* handleData = reinterpret_cast<HandleData*>( socket->uv_handle->data );

	// Make sure only one tasklet is receiving packets at a time because:
	// 1. We do 2 receives, and assume the first thing we receive is the length of a packet,
	//    and the second thing is the contents of that package.
	// 2. If we try to call uv_read_start twice in a row, we get error WSAEALREADY.
	handleData->activePacketReceiveRequests += 1;
	ON_BLOCK_EXIT( [&socket, &handleData] {
		if ( is_valid_socket( socket ) )
		{
			handleData->activePacketReceiveRequests -= 1;
			if( PyChannel_GetBalance( handleData->packetReceiveQueue ) < 0 )
			{
				int ret = PyChannel_Send( handleData->packetReceiveQueue, Py_None );
				if( ret == -1 )
				{
					LogError("ReceivePacket failed to send sentinel");
					PyErr_Clear();
				}
			}
		}
	} );
	if( handleData->activePacketReceiveRequests > 1 )
	{
		PyObject* ret = PyChannel_Receive( handleData->packetReceiveQueue );
		if( !ret )
		{
			return nullptr;
		}
	}

	uint32_t header{0};
	auto* request = new StreamRecvRequest(socket, sizeof(header), 0);
	auto* pyHeader = request->execute();
	if ( !pyHeader )
	{
		return nullptr;
	}
	ON_BLOCK_EXIT([pyHeader]{ Py_DecRef( pyHeader ); });
	if( !is_valid_socket( socket ) )
	{
		// The request completed successfully, but there is still
		// room for the socket to have closed between the sending
		// tasklet sending the data and this tasklet getting run
		// after the data is made available on the channel.
		PyErr_SetString(PyExc_OSError, "Socket closed while receiving packet");
		return nullptr;
	}

	header = ntohl( *reinterpret_cast<decltype( header )*>( PyBytes_AsString( pyHeader ) ) );
	uint32_t payloadLen = header & ceHeaderSizeMask;

	if ( payloadLen > handleData->maxPacketSize )
	{
		PyErr_Format(PyExc_OSError, "too large a packet detected at %d bytes, max is %llu", payloadLen, handleData->maxPacketSize);
		return nullptr;
	}

	uint32_t remaining = payloadLen;
	request = new StreamRecvRequest(socket, payloadLen, 0);
	auto* pyPayload = request->execute();
	if ( !pyPayload )
	{
		return nullptr;
	}
	remaining -= PyBytes_Size( pyPayload );
	while( remaining > 0 )
	{
		request = new StreamRecvRequest( socket, remaining, 0 );
		auto* chunk = request->execute();
		if( !chunk )
		{
			return nullptr;
		}
		remaining -= PyBytes_Size( chunk );
		PyBytes_ConcatAndDel( &pyPayload, chunk );
		if( !pyPayload )
		{
			return nullptr;
		}
	}

	char* payload = PyBytes_AsString( pyPayload );
	char* payloadEnd = payload + payloadLen;
	uint32_t oobDataLen{0};

	if ( (header & ceHeaderExpectPayloadOffset) == ceHeaderExpectPayloadOffset)
	{
		oobDataLen = ntohl( *reinterpret_cast<decltype( oobDataLen )*>( payload ) );

		if ( oobDataLen > handleData->maxPacketSize )
		{
			PyErr_Format(PyExc_OSError, "corrupted out-of-band data in packet detected at %d bytes, max is %llu", oobDataLen, handleData->maxPacketSize);
			return nullptr;
		}

		payload += sizeof(oobDataLen);
		auto* oobData = payload;
		for ( auto callback : s_oobDataCallbacks ) {
			auto stop = callback(
				static_cast<long long>( socket->sock_fd ),
				payload + oobDataLen,
				payloadLen - oobDataLen,
				oobData,
				oobDataLen
			);
			if (stop != 0) {
				// BlueNet ate the packet, so reset our internal state ... is this correct? When would bluenet eat the packet?
				Py_RETURN_NONE;
			}
		}
		payload += oobDataLen;
	}

	s_packetsReceived += 1;
	Py_IncRef( Py_None );
	auto* packet = PyTuple_Pack( 3, PyBytes_FromStringAndSize( payload, payloadEnd - payload ), Py_None, PyLong_FromSize_t( handleData->packetNumber++ ) );
	if ( !packet ) {
		Py_DecRef( Py_None );
	}
	return packet;
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
	delete bufs; // clean up the buffer array
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
	bufs[0] = uv_buf_init( (char*)data, len );
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
	size_t outlen = FormatPacket( buf, data, len, OOBData, OOBLen );
	if ( outlen == 0 )
	{
		delete buf;
		return 0;
	}
	// SendFormattedPacket takes ownership of `buf` at this point
	return SendFormattedPacket( fd, buf, outlen );
}

extern "C" int FormatPacket( char* buf, const char* data, uint32_t dataLen, const char* OOBData, uint32_t OOBLen )
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
