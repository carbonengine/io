#ifndef CARBONIO_H
#define CARBONIO_H

#include <array>

#include <Python.h>
#include <stackless_api.h>

#include <uv.h>

#include <CcpScopeGuard.h>
#include <BluePyCpp.h>

void cleanup_uv_handle( uv_handle_t* uv_handle );

void PyErr_FromUvErr( int error );
void PyWriteUnraisable( const char* msg );


enum ChannelPreference : int {
	PREFER_RECEIVER = -1,
	PREFER_NONE,
	PREFER_SENDER,
};

extern void SendError(PyChannelObject* channel, std::string_view msg);

extern void alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf);

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

class IStreamRequest : public IRequest
{
public:
	IStreamRequest( uv_stream_t* handle ) : IRequest( reinterpret_cast<uv_handle_t*>( handle ) ){}
	uv_stream_t* handle() { return reinterpret_cast<uv_stream_t*>( m_handle ); }
};

class StreamRecvRequest : public IStreamRequest
{
public:
	StreamRecvRequest( uv_stream_t* handle ) : IStreamRequest( handle ){}
	~StreamRecvRequest(){ Py_XDECREF(m_data);}
	PyObject* receive(Py_ssize_t length, int flags);
	uv_stream_t* handle() { return reinterpret_cast<uv_stream_t*>( m_handle ); }

	static void readCallback( uv_stream_t* client, ssize_t nread, const uv_buf_t* buf );

private:
	void onReceive( ssize_t nread, const uv_buf_t* buf );

	Py_ssize_t m_requested_len{0};
	Py_ssize_t m_received_len{0};
	int m_flags{0};
	PyObject* m_data{nullptr};
	Py_ssize_t m_pos{0};
};

class StreamSendRequest : public IStreamRequest
{
	public:
		StreamSendRequest( uv_stream_t* handle, char* buf, Py_ssize_t len, int flags ) :
			IStreamRequest( handle ), m_buf(buf), m_len(len), m_flags(flags) {}
		PyObject* send();
		static void sendCallback(uv_write_t* request, int status);
	private:
		void onSend( int status );

		char* m_buf;
		Py_ssize_t m_len;
		int m_flags;
};

class IUdpRequest : public IRequest
{
public:
	IUdpRequest( uv_udp_t* handle ) : IRequest( reinterpret_cast<uv_handle_t*>( handle ) ){}
	uv_udp_t* handle() { return reinterpret_cast<uv_udp_t*>( m_handle ); }
};

class UdpRecvRequest : public IUdpRequest
{
public:
	UdpRecvRequest( uv_udp_t* handle, Py_ssize_t len, int flags ) :
		IUdpRequest( handle ), m_len( len ), m_flags( flags )
	{
	}

	PyObject* receive();
	static void receiveCallback( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags );

private:
	void onRead( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags );

	Py_ssize_t m_len;
	int m_flags;
	PyObject* m_buf{nullptr};
	PyObject* m_addr{nullptr};
};

class UdpSendRequest : public IUdpRequest
{
public:
	UdpSendRequest( uv_udp_t* handle, char* buf, ssize_t len, const struct sockaddr* addr, int addrlen , int flags )
	: IUdpRequest( handle ), m_buf( buf ), m_len( len ), m_addr( addr ), m_addrLen( addrlen ) , m_flags( flags )
	{
	}

	PyObject* send();
	static void sendCallback(uv_udp_send_t* request, int status);

private:
	void onSend( int status );

	char* m_buf;
	ssize_t m_len;
	const struct sockaddr* m_addr;
	int m_addrLen;
	int m_flags;
};

#endif // CARBONIO_H
