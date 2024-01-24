#include <carbonio.h>

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
		auto ret = uv_read_start( handle(), StreamRecvRequest::alloc, StreamRecvRequest::readCallback );
		if( ret < 0 )
		{
			PyErr_FromUvErr( ret );
			return nullptr;
		}
	}
	PyChannel_SetPreference( channel(), PREFER_SENDER );
	auto sentinel = PyChannel_Receive( channel() );
	if( !sentinel )
	{
		return nullptr;
	}
	Py_DecRef( sentinel );
	PyChannel_SetPreference( channel(), PREFER_RECEIVER );

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
		if(m_received_len >= m_requested_len)
		{
			if ( PyChannel_Send( channel(), Py_None ) < 0 ) {
				PyWriteUnraisable( "StreamRecvRequest::onReceive failed to signal sentinel" );
			}
		}
	}
}

void StreamRecvRequest::alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf)
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
