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

PyObject* StreamRecvRequest::execute()
{
	auto ret = uv_read_start( handle(), StreamRecvRequest::alloc, StreamRecvRequest::readCallback );
	if ( ret < 0 ) {
		PyErr_FromUvErr( ret );
		return nullptr;
	}
	auto sentinel = PyChannel_Receive( m_channel );
	if ( ! sentinel ) {
		return nullptr;
	}
	Py_DecRef( sentinel );

	return m_data;
}

void StreamRecvRequest::onReceive( ssize_t nread, const uv_buf_t* buf )
{
	if ( nread < 0 ) {
		if (nread != UV_EOF) {
			PyErr_FromUvErr( int( nread ) );
		}
//		uv_close((uv_handle_t*) m_handle, on_close);
	}
	if ( nread > 0 ) {
		if ( ! m_data ) {
			m_data = PyBytes_FromStringAndSize( buf->base, buf->len );
		} else {
			PyBytes_ConcatAndDel(&m_data, PyBytes_FromStringAndSize( buf->base, buf->len ) );
			if ( ! m_data ) {
				// concatenation failed
			}
		}
		delete[] buf;
	}

	Py_INCREF( Py_None );
	if ( PyChannel_Send( m_channel, Py_None ) < 0 ) {
		PyWriteUnraisable( "StreamRecvRequest::onReceive failed to signal sentinel" );
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
