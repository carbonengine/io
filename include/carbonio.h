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

struct IRequest;
struct HandleData
{
	HandleData();
	~HandleData();

	// This will always be the channel that we're looking for
	PyChannelObject* channel;

	// This will always point to the associated request while there is one
	IRequest* request;

	// This backing buffer needs to outlive any potential request so that
	// we can correctly re-construct multiple `receive()` requests.
	uv_buf_t buf;

	// When receiving on a socket, libuv returns us more data than might have
	// been requested by the user. Since the buffer outlives the request, then
	// we also need to do some bookkeeping on where we are in the buffer.
	//
	// bufReadPos is the offset into the buffer to the data which will be returned to
	// the user on the next receive call.
	//
	// bufWritePos points to the end of the data we have on hand, but have not
	// returned to the user yet.
	ssize_t bufReadPos{0};
	ssize_t bufWritePos{0};
};

enum ChannelPreference : int {
	PREFER_RECEIVER = -1,
	PREFER_NONE,
	PREFER_SENDER,
};

extern void SendError(PyChannelObject* channel, std::string_view msg);

extern void alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf);
static Py_tss_t UV_LOOP_KEY = Py_tss_NEEDS_INIT;

extern void SetTimeoutErrorType(PyObject* error_type);


extern uv_loop_t * get_uv_loop();

void* create_handle_data();

extern "C" typedef struct PySocketSockObject_t PySocketSockObject;

struct IRequest
{
public:
	IRequest( PySocketSockObject* socket );

	virtual ~IRequest()
	{
		if( m_handle->data )
		{
			reinterpret_cast<HandleData*>( m_handle->data )->request = nullptr;
		}
		Py_DECREF(m_channel);
	}

	PyChannelObject* channel() const
	{
		return m_channel;
	}

	virtual void cancel();

	int startTimeout();

	static void timeoutCallback(uv_timer_t* result);

	virtual void onTimeout();

protected:
	void sendError(std::string_view msg);

	PyChannelObject* m_channel{nullptr};
	uv_handle_t* m_handle{nullptr};
	uv_timer_t* m_timeout{nullptr};
	_PyTime_t m_timeout_nanoseconds{-1};
};

class IStreamRequest : public IRequest
{
public:
	IStreamRequest( PySocketSockObject* socket ) : IRequest( socket ){}
	uv_stream_t* handle() { return reinterpret_cast<uv_stream_t*>( m_handle ); }
};

class StreamRecvRequest : public IStreamRequest
{
public:
	StreamRecvRequest( PySocketSockObject* socket );
	PyObject* receive(Py_ssize_t length, int flags);
	uv_stream_t* handle() { return reinterpret_cast<uv_stream_t*>( m_handle ); }
	void onTimeout() override;
	void cancel() override;

private:
	static void readCallback( uv_stream_t* client, ssize_t nread, const uv_buf_t* buf );
	void onReceive( ssize_t nread, const uv_buf_t* buf );
	static void alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf);

	Py_ssize_t m_requested_len{0};
	Py_ssize_t m_received_len{0};
	int m_flags{0};
};

class StreamSendRequest : public IStreamRequest
{
public:
	StreamSendRequest( PySocketSockObject* socket, char* buf, Py_ssize_t len, int flags ) :
		IStreamRequest( socket ), m_buf( buf ), m_len( len ), m_flags( flags )
	{
	}
	PyObject* send();
	static void sendCallback( uv_write_t* request, int status );

private:
		void onSend( int status );

		char* m_buf;
		Py_ssize_t m_len;
		int m_flags;
};

class IUdpRequest : public IRequest
{
public:
	IUdpRequest( PySocketSockObject* socket ) : IRequest( socket ){}
	uv_udp_t* handle() { return reinterpret_cast<uv_udp_t*>( m_handle ); }
};

class UdpRecvRequest : public IUdpRequest
{
public:
	UdpRecvRequest( PySocketSockObject* socket, Py_ssize_t len, int flags ) :
		IUdpRequest( socket ), m_len( len ), m_flags( flags )
	{
	}

	PyObject* receive();
	void cancel() override;
	static void receiveCallback( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags );

private:
	void onRead( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags );
	void onTimeout() override;

	Py_ssize_t m_len;
	int m_flags;
	PyObject* m_buf{nullptr};
	PyObject* m_addr{nullptr};
};

class UdpSendRequest : public IUdpRequest
{
public:
	UdpSendRequest( PySocketSockObject* socket, char* buf, ssize_t len, const struct sockaddr* addr, int addrlen , int flags )
	: IUdpRequest( socket ), m_buf( buf ), m_len( len ), m_addr( addr ), m_addrLen( addrlen ) , m_flags( flags )
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


class StreamAcceptRequest : IStreamRequest
{
public:
	StreamAcceptRequest( PySocketSockObject* socket ) :
		IStreamRequest( socket )
	{
	}

	PyObject* accept();
};


#endif // CARBONIO_H
