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
static Py_tss_t UV_LOOP_KEY = Py_tss_NEEDS_INIT;

extern void SetTimeoutErrorType(PyObject* error_type);


extern uv_loop_t * get_uv_loop();

extern "C" typedef struct PySocketSockObject_t PySocketSockObject;

struct IRequest
{
public:
	IRequest( PySocketSockObject* socket );

	virtual ~IRequest()
	{
		m_handle->data = m_channel;
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

	// A libuv callback may fire after a timeout has occurred. At this point,
	// the data member on the request may no longer point to a request, but to
	// a stackless channel instead. Check the tag on the pointer to see if the
	// data still points to a valid request.
	static bool timedOut( void* maybe_request );

	// The libuv handle has a single data member on which the user can store application specific data.
	// We use this data member to serve two purposes:
	// 1. When the handle is created we store the stackless channel for the request
	//    in there. The channel continues to live there except during the execution of use case 2.
	// 2. When we register for callbacks from libuv, we store the request itself in the data attribute. Once
	//    the callback is finished, we set the attribute back to the channel.
	//
	// These use-cases conflict when we receive uv callbacks from outside the context of a request
	// For example on_accept and cleanup_uv_handle need access to the channel, but at any given time may either
	// be holding a pointer to a request class or to a stackless channel.
	//
	// Here we use the "tagged pointer" pattern to solve this problem.
	// https://en.wikipedia.org/wiki/Tagged_pointer

	static void* untag( void* tagged_pointer );
	static bool isTagged( void* tagged_pointer );
	static PyChannelObject* ChannelPtr( void* tagged_pointer );

	static constexpr uint64_t TAG = 0x1000000000000000L;

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
	~StreamRecvRequest(){ Py_XDECREF(m_data);}
	PyObject* receive(Py_ssize_t length, int flags);
	uv_stream_t* handle() { return reinterpret_cast<uv_stream_t*>( m_handle ); }
	void onTimeout() override;
	void cancel() override;
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
