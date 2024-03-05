#include <carbonio.h>
#include "socketmodule.h"

#ifdef __APPLE__
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

int InitUvLoop() {
	// uv_loop instances aren't thread-safe, thus we keep a loop instance per thread for which we need to initialize TLS
	auto status = uv_key_create(&s_tlsKey);
	if ( status != 0 ) {
		PyErr_FromUvErr( status );
	}
	return status;
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

HandleData::HandleData() : channel( PyChannel_New( nullptr ) ), request(nullptr)
{
	buf = uv_buf_init( nullptr, 0 );
}

HandleData::~HandleData()
{
	Py_XDECREF( channel );
	channel = nullptr;
	delete buf.base;
	buf.base = nullptr;
	buf.len = 0;
	bufReadPos = -1;
	bufWritePos = -1;
}


void* create_handle_data()
{
	auto* data = new HandleData;
	if( data->channel == nullptr )
	{
		delete data;
		return nullptr;
	}
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

void PyWriteUnraisable( const char* msg )
{
	PyObject *exc, *val, *tb;
	PyObject* msg_obj;
	PyErr_Fetch( &exc, &val, &tb );
	msg_obj = PyUnicode_FromString( msg ? msg : "" );
	PyErr_Restore( exc, val, tb );
	if( !msg_obj )
	{
		msg_obj = Py_None;
		Py_INCREF( Py_None );
	}
	PyErr_WriteUnraisable( msg_obj );
	Py_DECREF( msg_obj );
}

uv_loop_t * get_uv_loop()
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
void SetTimeoutErrorType(PyObject* value)
{
	s_timeout_error = value;
}

IRequest::IRequest( PySocketSockObject* socket ) : m_handle( socket->uv_handle ), m_timeout_nanoseconds(socket->sock_timeout)
{
	handleData()->request.reset(this);
	m_self = handleData()->request;
}

void IRequest::sendError(std::string_view msg)
{
	PyObject *exc, *val, *tb;
	PyErr_Fetch( &exc, &val, &tb );
	PyChannel_SetPreference(handleData()->channel, PREFER_SENDER );
	auto ret = PyChannel_SendThrow( handleData()->channel, exc, val, tb);
	if( ret < 0 )
	{
		PyErr_Restore( exc, val, tb );
		PyWriteUnraisable( msg.data() );
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
	uv_timer_init( get_uv_loop(), m_timeout );
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
		auto sentinel = PyChannel_Receive( data->channel );
		if( !sentinel )
		{
			return nullptr;
		}
	}
	data->bufWritePos += m_received_len;

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
	Ccp::PyGilEnsure gil;
	if( nread == 0 ) {
		return;
	}
	ON_BLOCK_EXIT( [&] { clearTimeout(); finalize();} );
	PyChannel_SetPreference(handleData()->channel, PREFER_SENDER );
	if ( nread < 0 ) {
		if (nread != UV_EOF) {
			PyErr_FromUvErr( int( nread ) );
			sendError("OnReceive failed to read data.");
		}
		else {
			if ( PyChannel_Send( handleData()->channel, Py_None ) < 0 ) {
				PyWriteUnraisable( "StreamRecvRequest::onReceive failed to signal sentinel" );
			}
		}
	}
	if ( nread > 0 ) {
		m_received_len += nread;
		uv_read_stop( handle() );
		if ( PyChannel_Send( handleData()->channel, Py_None ) < 0 ) {
			PyWriteUnraisable( "StreamRecvRequest::onReceive failed to signal sentinel" );
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
	if( data->request )
	{
		auto _this = reinterpret_cast<StreamRecvRequest*>( data->request.get() );
		auto params = std::make_unique<StreamRecvRequest::Params>(nread, buf);
		_this->onCallback( params.get() );
	}
}

void StreamRecvRequest::cancel()
{
	uv_read_stop( handle() );
	// Check the balance, as this could be called after the request has finished executing.
	if( PyChannel_GetBalance( handleData()->channel ) < 0 )
	{
		if( PyChannel_Send( handleData()->channel, Py_None ) < 0 )
		{
			PyWriteUnraisable( "StreamRecvRequest::cancel failed to signal sentinel" );
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
	return uv_read_start( handle(), StreamRecvRequest::alloc, StreamRecvRequest::readCallback );
}

PyObject* StreamSendRequest::execute()
{
	if ( ! startTimeout() )
	{
		return nullptr;
	}
	m_writeRequest.data = this;
	int status = uv_write(&m_writeRequest, handle(), &m_sendBuffer, 1, StreamSendRequest::sendCallback );
	if( status < 0 ){
		return PyLong_FromLong(status);
	}

	if( handleData()->blockingSend )
	{
		return PyChannel_Receive( handleData()->channel );
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
	ON_BLOCK_EXIT( [&] { clearTimeout(); finalize();} );
	auto *params = dynamic_cast<StreamSendRequest::Params*>(callbackParams);
	Ccp::PyGilEnsure gil;

	auto py_status = PyLong_FromLong(params->status);
	if( !py_status ){
		sendError("StreamSendRequest::send Failed to convert status to python int");
		return;
	}
	if( handleData()->blockingSend )
	{
		if( PyChannel_Send( handleData()->channel, py_status ) < 0 )
		{
			PyWriteUnraisable( "StreamSendRequest::send Failed to send status over channel" );
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
		PyWriteUnraisable( msg.data() );
	}
}

PyObject* UdpRecvRequest::execute()
{
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

	auto sentinel = PyChannel_Receive( handleData()->channel );
	if( !sentinel )
	{
		return nullptr;
	}

	return PyTuple_Pack( 2, m_buf, m_addr );
}

void UdpRecvRequest::receiveCallback( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags )
{
	auto* data = reinterpret_cast<HandleData*>( handle->data );
	if( data->request )
	{
		auto _this = reinterpret_cast<UdpRecvRequest*>( data->request.get() );
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
		if ( PyChannel_Send( handleData()->channel, Py_None ) < 0 )
		{
			PyWriteUnraisable( "UdpRecvRequest::onRead failed sending sentinel value on channel" );
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
	if( PyChannel_GetBalance( handleData()->channel ) < 0 )
	{
		if( PyChannel_Send( handleData()->channel, Py_None ) < 0 )
		{
			PyWriteUnraisable( "UdpRecvRequest::cancel failed sending sentinel value on channel" );
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
	auto ret = PyChannel_Receive( handleData()->channel );
	status = PyLong_AsLong( ret );
	if ( status < 0 ) {
		if( !( status == -1 && PyErr_Occurred() ) )
		{
			PyErr_FromUvErr( status );
		}
		return nullptr;
	}
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
	ON_BLOCK_EXIT( [&] { finalize();} );
	auto *params = dynamic_cast<UdpSendRequest::Params*>(callbackParams);
	auto py_status = PyLong_FromLong( params->status );
	if( !py_status )
	{
		sendError( "UdpSendRequest::send Failed to convert status to python int" );
		return;
	}
	if( PyChannel_Send( handleData()->channel, py_status ) < 0 )
	{
		PyWriteUnraisable( "UdpSendRequest::send Failed to send status over channel" );
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

	result = PyChannel_Receive( handleData()->channel );

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
}

PyObject* StreamConnectRequest::execute()
{
	auto* connect = new uv_connect_t;
	ON_BLOCK_EXIT( [&connect] { delete connect; } );
	Py_XDECREF(startTimeout());
	int status = uv_tcp_connect(connect, reinterpret_cast<uv_tcp_t*>( handle() ), m_address, &StreamConnectRequest::connectCallback);
	if ( status < 0 )
	{
		PyErr_FromUvErr( status );
		return nullptr;
	}
	PyObject* connect_status = PyChannel_Receive(handleData()->channel);
	if( connect_status == nullptr ) {
		return nullptr;
	}
	if( !PyLong_Check( connect_status ) ) {
		PyErr_BadInternalCall();
		return nullptr;
	}
	status = PyLong_AsLong( connect_status );
	if( status < 0 ) {
		PyErr_FromUvErr( status );
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
	ON_BLOCK_EXIT( [this] { clearTimeout(); finalize();} );
	Ccp::PyGilEnsure gil;
	auto *params = dynamic_cast<StreamConnectRequest::Params*>(callbackParams);

	auto py_status = PyLong_FromLong( params->status );
	if( py_status == nullptr )
	{
		PyObject *exc, *val, *tb;
		PyErr_Fetch( &exc, &val, &tb );
		auto ret = PyChannel_SendThrow( handleData()->channel, exc, val, tb );
		if( ret < 0 )
		{
			PyErr_Restore( exc, val, tb );
			PyWriteUnraisable( "StreamConnectRequest::onConnect failed to send exception" );
		}
		return;
	}
	int ret = PyChannel_Send( handleData()->channel, py_status );
	if( ret < 0 )
	{
		PyWriteUnraisable( "StreamConnectRequest::onConnect failed to send status" );
	}
}
