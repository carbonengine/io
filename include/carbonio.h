#ifndef CARBONIO_H
#define CARBONIO_H

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
	std::shared_ptr<IRequest> request;

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
	bool blockingSend{true};
	size_t maxPacketSize{1024*1024}; //one megabyte

	// in case the socket deals with packets, it needs to keep track of a packet's sequence number.
	size_t packetNumber{0};
};

enum ChannelPreference : int {
	PREFER_RECEIVER = -1,
	PREFER_NONE,
	PREFER_SENDER,
};

extern void SendError(PyChannelObject* channel, std::string_view msg);

extern void alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf);

extern void SetTimeoutErrorType(PyObject* error_type);


extern int InitUvLoop();
extern uv_loop_t * get_uv_loop();
extern PyObject* GetStatistics();

void* create_handle_data();

extern "C" typedef struct PySocketSockObject_t PySocketSockObject;

extern PyObject* ReceivePacket( PySocketSockObject* socket );
extern PyObject* SendPacket( PySocketSockObject* socket, void* data, Py_ssize_t len );

struct ICallbackParams {
	virtual ~ICallbackParams() = default;
};

struct IRequest
{
public:
	IRequest( PySocketSockObject* socket );

	virtual ~IRequest()
	{
		clearTimeout();
	}

	virtual void cancel();

	[[nodiscard]] PyObject* startTimeout();

	static void timeoutCallback(uv_timer_t* result);

	virtual void onTimeout();

	virtual PyObject* execute() = 0;

	virtual void onCallback( ICallbackParams* params ) = 0;

protected:
	void clearTimeout();
	HandleData* handleData() { return reinterpret_cast<HandleData*>(m_handle->data); }
	void sendError(std::string_view msg);
	void finalize() { m_self.reset(); }

	uv_handle_t* m_handle{nullptr};
	uv_timer_t* m_timeout{nullptr};
	_PyTime_t m_timeout_nanoseconds{-1};
	bool m_timedOut{false};

	// We keep around a shared pointer to ourselves because we need the request to live until
	// the libuv callbacks for that request have finished, which in some cases is after the
	// `execute()` method has returned.
	// Once no more libuv callbacks are expected for the request, `finalize()` needs to
	// be called in order to avoid leaking requests.
	std::shared_ptr<IRequest> m_self{nullptr};
};

class IStreamRequest : public IRequest
{
public:
	IStreamRequest( PySocketSockObject* socket ) : IRequest( socket ){}
	uv_stream_t* handle() { return reinterpret_cast<uv_stream_t*>( m_handle ); }
};

class StreamConnectRequest : public IStreamRequest
{
public:
	StreamConnectRequest( PySocketSockObject* socket, struct sockaddr* address );
	PyObject* execute() override;

	static void connectCallback(uv_connect_t* connection, int status);

	struct Params : public ICallbackParams {
		int status;
		Params(int status) : status(status) {};
	};

private:
	void onCallback (ICallbackParams* status) override;

	struct sockaddr* m_address{ nullptr };
};

class StreamRecvRequest : public IStreamRequest
{
public:
	StreamRecvRequest( PySocketSockObject* socket, Py_ssize_t length, int flags );
	PyObject* execute() override;
	uv_stream_t* handle() { return reinterpret_cast<uv_stream_t*>( m_handle ); }
	void onTimeout() override;
	void cancel() override;

	struct Params : public ICallbackParams {
		ssize_t nread;
		const uv_buf_t* buf;
		Params(ssize_t nread, const uv_buf_t* buf) : nread(nread), buf(buf) {};
	};

protected:
	static void readCallback( uv_stream_t* client, ssize_t nread, const uv_buf_t* buf );
	virtual int startRead();
	virtual PyObject* constructResult( HandleData* data ) const;

	Py_ssize_t m_requested_len{0};
	Py_ssize_t m_received_len{0};
	int m_flags{0};

private:
	void onCallback( ICallbackParams *callbackParams ) override;
	static void alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf);
};

class StreamRecvIntoRequest : public StreamRecvRequest
{
public:
	StreamRecvIntoRequest(PySocketSockObject* s, char* buf, Py_ssize_t length, int flags);
private:
	int startRead() override;
	static void alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf);
	PyObject* constructResult( HandleData* data ) const override;

	char* m_buf{nullptr};
};

class StreamSendRequest : public IStreamRequest
{
public:
	StreamSendRequest( PySocketSockObject* socket, char* buf, Py_ssize_t len, int flags ) :
		IStreamRequest( socket ), m_flags( flags )
	{
		m_sendBuffer.base = buf;
		m_sendBuffer.len = len;
	}
	PyObject* execute() override;
	static void sendCallback( uv_write_t* request, int status );

	struct Params : public ICallbackParams {
		int status;
		Params(int status) : status(status) {};
	};

private:
		void onCallback( ICallbackParams *callbackParams ) override;
		int m_flags;
		uv_write_t m_writeRequest;
		uv_buf_t m_sendBuffer;
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

	PyObject* execute() override;
	void cancel() override;
	static void receiveCallback( uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags );
	struct Params : public ICallbackParams {
		ssize_t nread;
		const uv_buf_t* buf;
		const struct sockaddr* addr;
		unsigned flags;
		Params( ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags ) : nread(nread), buf(buf), addr(addr), flags(flags) {};
	};

private:
	void onCallback( ICallbackParams *callbackParams ) override;
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
	: IUdpRequest( socket ), m_addr( addr ), m_addrLen( addrlen ) , m_flags( flags )
	{
		m_sendBuffer.base = buf;
		m_sendBuffer.len = len;
	}

	PyObject* execute() override;
	static void sendCallback(uv_udp_send_t* request, int status);

	struct Params : public ICallbackParams {
		int status;
		Params(int status) : status(status) {};
	};

private:
	void onCallback( ICallbackParams *callbackParams ) override;

	const struct sockaddr* m_addr;
	int m_addrLen;
	int m_flags;

	uv_udp_send_t m_writeRequest;
	uv_buf_t m_sendBuffer;
};


class StreamAcceptRequest : IStreamRequest
{
public:
	StreamAcceptRequest( PySocketSockObject* socket ) :
		IStreamRequest( socket )
	{
	}

	PyObject* execute() override;
private:
	void onCallback(ICallbackParams *params) override {};
};


#endif // CARBONIO_H
