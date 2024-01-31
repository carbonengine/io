#include <carbonio.h>
#include "socketmodule.h"

void cleanup_uv_handle( uv_handle_t* uv_handle )
{
	Ccp::PyGilEnsure gil;
	if ( IRequest::isTagged( uv_handle->data ) ) {
		reinterpret_cast<IRequest*>(IRequest::untag( uv_handle->data ) )->cancel();
	}

	Py_XDECREF( IRequest::ChannelPtr( uv_handle->data ) );
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

void PyErr_FromUvErr( int error )
{
	PyErr_SetString( PyExc_OSError, uv_err_name( error ) );
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
	Ccp::PyGilEnsure gil;
	uv_loop_t* ret = reinterpret_cast<uv_loop_t*>(PyThread_tss_get(&UV_LOOP_KEY));
	if ( !ret ) {
		ret = new uv_loop_t;
		auto res = uv_loop_init( ret );
		if ( res < 0 ) {
			uv_loop_delete( ret );
			PyErr_FromUvErr( res );
			return nullptr;
		}
		if ( PyThread_tss_set( &UV_LOOP_KEY, reinterpret_cast<void *>( ret ) ) ) {
			uv_loop_close( ret );
			delete ret;
			return nullptr;
		}
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
	m_channel = reinterpret_cast<PyChannelObject*>( m_handle->data );
	Py_IncRef( reinterpret_cast<PyObject*>( m_channel ) );
	m_handle->data = this;
	m_handle->data = reinterpret_cast<void*>(uint64_t(m_handle->data) | TAG);
}

void IRequest::sendError(std::string_view msg)
{
	PyObject *exc, *val, *tb;
	PyErr_Fetch( &exc, &val, &tb );
	auto ret = PyChannel_SendThrow( channel(), exc, val, tb);
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

int IRequest::startTimeout()
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
		return 0;
	}
	uint64_t timeout_ms = m_timeout_nanoseconds / 1000000;
	m_timeout = new uv_timer_t;
	m_timeout->data = this;
	uv_timer_init( get_uv_loop(), m_timeout );
	return uv_timer_start(m_timeout, timeoutCallback, timeout_ms, 0);
}

void IRequest::onTimeout()
{
	Ccp::PyGilEnsure gil;
	if( m_timeout_nanoseconds == 0 )
	{
		// The Python socket module returns an OSError when making blocking operations in non-blocking mode,
		// which can be set either by calling setblocking(False) or setting the timeout to 0.0.
		PyErr_SetString(PyExc_OSError, "Attempted a blocking operation on a non-blocking socket");
	}
	else
	{
		PyErr_SetString( s_timeout_error, "timed out" );
	}
	sendError("IRequest::onTimeout failed to send timeout exception");
}


void* IRequest::untag( void* tagged_pointer )
{
	return reinterpret_cast<void*>( uint64_t( tagged_pointer ) & ~TAG );
}

PyChannelObject* IRequest::ChannelPtr( void* tagged_pointer )
{
	if( isTagged(tagged_pointer) )
	{
		auto request = reinterpret_cast<IRequest*>( untag( tagged_pointer ) );
		return request->channel();
	}
	return reinterpret_cast<PyChannelObject*>(tagged_pointer);
}

bool IRequest::isTagged( void* tagged_pointer )
{
	return uint64_t( tagged_pointer ) & TAG;
}
bool IRequest::timedOut( void* maybe_request )
{
	return !isTagged(maybe_request);
}
void IRequest::cancel()
{
	if( m_timeout )
	{
		uv_timer_stop( m_timeout );
	}
}


PyObject* StreamRecvRequest::receive( Py_ssize_t length, int flags )
{
	m_requested_len = length;
	m_flags = flags;
	if( !m_data )
	{
		PyErr_BadInternalCall();
		return nullptr;
	}
	if ( PyBytes_Size( m_data ) == 0 )
	{
		auto ret = uv_read_start( handle(), alloc, StreamRecvRequest::readCallback );
		if( ret < 0 )
		{
			PyErr_FromUvErr( ret );
			return nullptr;
		}
		ret = startTimeout();
		if( ret < 0 )
		{
			uv_read_stop(handle());
			PyErr_FromUvErr( ret );
			return nullptr;
		}
		auto sentinel = PyChannel_Receive( channel() );
		if( !sentinel )
		{
			return nullptr;
		}
	}
	auto remaining_data_length = PyBytes_GET_SIZE(m_data) - m_pos;
	auto chunk_size = remaining_data_length < m_requested_len ? remaining_data_length : m_requested_len;
	auto chunk = PyBytes_FromStringAndSize( PyBytes_AS_STRING(m_data) + m_pos, chunk_size);
	m_pos += chunk_size;

	return chunk;
}

void StreamRecvRequest::onReceive( ssize_t nread, const uv_buf_t* buf )
{
	Ccp::PyGilEnsure gil;
	if( nread == 0 ) {
		return;
	}
	if ( nread < 0 ) {
		if (nread != UV_EOF) {
			PyErr_FromUvErr( int( nread ) );
			sendError("OnReceive failed to read data.");
		}
		else {
			if ( PyChannel_Send( channel(), Py_None ) < 0 ) {
				PyWriteUnraisable( "StreamRecvRequest::onReceive failed to signal sentinel" );
			}
		}
		delete[] buf->base;
		//uv_close((uv_handle_t*) m_handle, cleanup_uv_handle);
	}
	if ( nread > 0 ) {
		PyBytes_ConcatAndDel(&m_data, PyBytes_FromStringAndSize( buf->base, nread ) );
		m_received_len += nread;
		if ( ! m_data ) {
			sendError("OnReceive failed to construct PyBytes object.");
		}
		if (m_received_len >= m_requested_len) {
			uv_read_stop( handle() );
			if ( PyChannel_Send( channel(), Py_None ) < 0 ) {
				PyWriteUnraisable( "StreamRecvRequest::onReceive failed to signal sentinel" );
			}
		}
	}
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
	if( !timedOut( client->data ) )
	{
		auto _this = reinterpret_cast<StreamRecvRequest*>( untag( client->data ) );
		_this->onReceive( nread, buf );
	}
}
void StreamRecvRequest::cancel()
{
	uv_read_stop( handle() );
	if( PyChannel_Send( channel(), Py_None ) < 0 )
	{
		PyWriteUnraisable( "StreamRecvRequest::cancel failed to signal sentinel" );
	}
	IRequest::cancel();
}

StreamRecvRequest::StreamRecvRequest( PySocketSockObject* socket ) :
	IStreamRequest( socket )
{
	m_data = PyBytes_FromStringAndSize( "", 0 );
	if ( !m_data ) {
		PyWriteUnraisable("StreamRecvRequest::StreamRecvRequest failed to allocate m_data");
	}
}

PyObject* StreamSendRequest::send()
{
	uv_write_t* request = new uv_write_t;
	constexpr int NUM_BUFFERS = 1;
	auto bufferarray = new std::array<uv_buf_t, NUM_BUFFERS> {{ULONG(m_len), m_buf}};
	int status = uv_write(request, handle(), bufferarray->data(), NUM_BUFFERS, StreamSendRequest::sendCallback );
	if( status < 0 ){
		delete bufferarray;
		delete request;
		return PyLong_FromLong(status);
	}
	auto ret = PyChannel_Receive(channel() );
	delete bufferarray;
	delete request;
	return ret;
}

void StreamSendRequest::sendCallback( uv_write_t* request, int status )
{
	if( !timedOut( request->handle->data ) )
	{
		auto _this = reinterpret_cast<StreamSendRequest*>( untag( request->handle->data ) );
		_this->onSend( status );
	}
}

void StreamSendRequest::onSend( int status )
{
	auto py_status = PyLong_FromLong(status);
	if( !py_status ){
		sendError("StreamSendRequest::send Failed to convert status to python int");
		return;
	}
	if( PyChannel_Send( channel(), py_status ) < 0 )
	{
		PyWriteUnraisable("StreamSendRequest::send Failed to send status over channel");
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

PyObject* UdpRecvRequest::receive()
{
	auto status = uv_udp_recv_start( handle(), alloc, UdpRecvRequest::receiveCallback );
	if ( status < 0 )
	{
		PyErr_FromUvErr( status );
		return nullptr;
	}

	status = startTimeout();
	if( status < 0 )
	{
		uv_udp_recv_stop(handle());
		PyErr_FromUvErr( status );
		return nullptr;
	}
	auto sentinel = PyChannel_Receive( channel() );
	if( !sentinel )
	{
		return nullptr;
	}

	return PyTuple_Pack( 2, m_buf, m_addr );
}

void UdpRecvRequest::receiveCallback( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags )
{
	if( !timedOut( handle->data ) )
	{
		auto _this = reinterpret_cast<UdpRecvRequest*>( untag( handle->data ) );
		_this->onRead( handle, nread, buf, addr, flags );
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

void UdpRecvRequest::onRead( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags )
{
	auto bufferGuard = MakeGuard([&] {delete buf;});
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
		if ( PyChannel_Send( channel(), Py_None ) < 0 )
		{
			PyWriteUnraisable( "UdpRecvRequest::onRead failed sending sentinel value on channel" );
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
	if( PyChannel_Send( channel(), Py_None ) < 0 )
	{
		PyWriteUnraisable( "UdpRecvRequest::cancel failed sending sentinel value on channel" );
	}
	IRequest::cancel();
}

PyObject* UdpSendRequest::send()
{
	auto* request = new uv_udp_send_t;
	ON_BLOCK_EXIT( [&] { delete request; } );

	constexpr int NUM_BUFFERS = 1;
	auto bufferarray = new std::array<uv_buf_t, NUM_BUFFERS>{ { ULONG( m_len ), m_buf } };
	ON_BLOCK_EXIT( [&] { delete bufferarray; } );

	int status = uv_udp_send( request, handle(), bufferarray->data(), NUM_BUFFERS, m_addr, UdpSendRequest::sendCallback );
	if( status < 0 )
	{
		return PyLong_FromLong( status );
	}
	auto ret = PyChannel_Receive( channel() );
	status = PyLong_AsLong( ret );
	if ( status < 0 ) {
		if( !( status == -1 && PyErr_Occurred() ) )
		{
			PyErr_FromUvErr( status );
		}
		return nullptr;
	}
	ret = PyLong_FromSsize_t(m_len);
	return ret;
}

void UdpSendRequest::sendCallback( uv_udp_send_t* request, int status )
{
	if( !timedOut( request->handle->data ) )
	{
		auto _this = reinterpret_cast<UdpSendRequest*>( untag( request->handle->data ) );
		_this->onSend( status );
	}
}

void UdpSendRequest::onSend( int status )
{
	auto py_status = PyLong_FromLong( status );
	if( !py_status )
	{
		sendError( "UdpSendRequest::send Failed to convert status to python int" );
		return;
	}
	if( PyChannel_Send( channel(), py_status ) < 0 )
	{
		PyWriteUnraisable( "UdpSendRequest::send Failed to send status over channel" );
	}
}

PyObject* StreamAcceptRequest::accept()
{
	startTimeout();
	auto result = PyChannel_Receive(channel());
	if( !result ) {
		return nullptr;
	}
	ON_BLOCK_EXIT( [&] { Py_XDECREF( result ); } );
	auto listen_status = PyTuple_GetItem(result, 0);

	if( !PyLong_Check( listen_status ) ) {
		PyErr_BadInternalCall();
		return nullptr;
	}
	ON_BLOCK_EXIT( [&] { Py_XDECREF( listen_status ); } );

	auto status = PyLong_AsLong( listen_status );
	if( status < 0 ) {
		if( !PyErr_Occurred() )
		{
			PyErr_FromUvErr( status );
		}
		return nullptr;
	}
	return PyTuple_GetItem(result, 1);
}
