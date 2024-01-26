#include <carbonio.h>
#include "socketmodule.h"

void cleanup_uv_handle( uv_handle_t* uv_handle )
{
	Ccp::PyGilEnsure gil;
	Py_XDECREF( uv_handle->data );
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

PyObject* StreamRecvRequest::receive( Py_ssize_t length, int flags )
{
	m_requested_len = length;
	m_flags = flags;
	if( !m_data )
	{
		auto ret = uv_read_start( handle(), alloc, StreamRecvRequest::readCallback );
		if( ret < 0 )
		{
			PyErr_FromUvErr( ret );
			return nullptr;
		}
		PyChannel_SetPreference( channel(), PREFER_SENDER );
		auto sentinel = PyChannel_Receive( channel() );
		if( !sentinel )
		{
			return nullptr;
		}
		Py_DecRef( sentinel );
		PyChannel_SetPreference( channel(), PREFER_RECEIVER );
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
		if ( ! m_data ) {
			m_data = PyBytes_FromStringAndSize( buf->base, nread );
		} else {
			PyBytes_ConcatAndDel(&m_data, PyBytes_FromStringAndSize( buf->base, nread ) );
		}
		m_received_len += nread;
		if ( ! m_data ) {
			sendError("OnReceive failed to construct PyBytes object.");
		}
	}
}

void alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf)
{
	// TODO what if allocation fails?!
	buf->base = new char[size];
	buf->len = ULONG(size);
}

void StreamRecvRequest::readCallback( uv_stream_t* client, ssize_t nread, const uv_buf_t* buf )
{
	auto _this = reinterpret_cast<StreamRecvRequest*>( client->data );
	_this->onReceive( nread, buf );
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
	PyChannel_SetPreference(channel(), PREFER_SENDER);
	auto ret = PyChannel_Receive(channel() );
	delete bufferarray;
	delete request;
	return ret;
}

void StreamSendRequest::sendCallback( uv_write_t* request, int status )
{
	auto _this = reinterpret_cast<StreamSendRequest*>( request->handle->data );
	_this->onSend( status );
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

	PyChannel_SetPreference( channel(), PREFER_SENDER );
	auto sentinel = PyChannel_Receive( channel() );
	if( !sentinel )
	{
		return nullptr;
	}

	return PyTuple_Pack( 2, m_buf, m_addr );
}

void UdpRecvRequest::receiveCallback( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags )
{
	auto _this = reinterpret_cast<UdpRecvRequest*>( handle->data );
	_this->onRead( handle, nread, buf, addr, flags );
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
	PyChannel_SetPreference( channel(), PREFER_SENDER );
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
	auto _this = reinterpret_cast<UdpSendRequest*>( request->handle->data );
	_this->onSend( status );
}

void UdpSendRequest::onSend( int status )
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
