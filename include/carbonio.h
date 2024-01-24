#ifndef CARBONIO_H
#define CARBONIO_H

#include <Python.h>
#include <stackless_api.h>

#include <uv.h>

#include <BluePyCpp.h>

void cleanup_uv_handle( uv_handle_t* uv_handle );

void PyErr_FromUvErr( int error );
void PyWriteUnraisable( const char* msg );


enum ChannelPreference : int {
	PREFER_RECEIVER = -1,
	PREFER_NONE,
	PREFER_SENDER,
};


struct IRequest
{
public:
	IRequest( uv_handle_t* handle ) : m_handle( handle )
	{
		m_channel = reinterpret_cast<PyChannelObject*>( m_handle->data );
		m_handle->data = this;
	}

	~IRequest()
	{
		m_handle->data = m_channel;
	}

	PyChannelObject* channel() const
	{
		return m_channel;
	}

protected:
	void sendError(std::string_view msg);

	PyChannelObject* m_channel{nullptr};
	uv_handle_t* m_handle{nullptr};
};

class StreamRecvRequest : public IRequest
{
public:
	StreamRecvRequest( uv_stream_t* handle ) : IRequest( reinterpret_cast<uv_handle_t*>( handle ) ){}
	~StreamRecvRequest(){ Py_XDECREF(m_data);}
	PyObject* receive(Py_ssize_t length, int flags);
	uv_stream_t* handle() { return reinterpret_cast<uv_stream_t*>( m_handle ); }

	static void alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf);
	static void readCallback( uv_stream_t* client, ssize_t nread, const uv_buf_t* buf );

private:
	void onReceive( ssize_t nread, const uv_buf_t* buf );

	Py_ssize_t m_requested_len{0};
	Py_ssize_t m_received_len{0};
	int m_flags{0};
	PyObject* m_data{nullptr};
	Py_ssize_t m_pos{0};
};

#endif // CARBONIO_H
