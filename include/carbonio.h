/*****************************************************************************
 * stacklessio.h
 * StacklessIO is a system for managing asynchronous requests for python
 * programs.  This involves generating "event" objects that are executed.
 * The requests then may "submit" themselves to a "event queue".  The requests
 * are then "dispatched" by the main python thread before being discarded.
 * The main application for this is in Stackless Python to perform blocking
 * operations by blocking "tasklets".  For example, a request to read from
 * disk may involve the following actions:
 * 1) The python main thread creates a ReadRequest (a subclass of IOEvent)
 *    object and kicks off its execution.  This may for example start a worker
 *    thread running.
 * 2) The tasklet in question blocks, e.g. by calling "receive" on a channel
 *    that is part of the request.
 * 3) the request finishes running and submits itself to the event queue.
 * 4) The event queue dispatches this request, which involves waking up the
 *    sleeping tasklet
 * Events may perform their execution any way they like.  In particular,
 * they may make use of worker threads that don't occupy the python GIL.
 * Submitting requests to the dispatcher is thread-safe and doesn't require
 * holding the GIL.
 *
 * The main idea behind this is to provide a central place for dipatching
 * asynchronous requests like this, the Event Queue.  This allows one to
 * devise a single scheduling policy for an application and ensure some
 * sense of fairness amoung multiple requests coming from multiple modules.
 * In the absence of this, various modules may design their own solutions
 * that can interact badly with one and other.
 *
 * StacklessIO is object oriented.  Events are required to inherit from
 * a virtual base class and implement a certain interface.  Specializations
 * are provided so that an application may need to implement only a single
 * virtual function.  This makes it very easy to implement such requests
 * from C++ Python code.  The classes are exported from the python DLL
 * for easy c++ linkage.
 *
 * Dispatching can be done in various ways.  In single threaded python it
 * can be done at convenient places, such as prior to going to sleep.  But
 * mainly it is supposed to be done at regular intervals, such as at the
 * start of the application loop.
 *
 * A facility for waking up the application is provided so that the
 * application can sleep until an event is ready.  Whenever the dispatcher
 * is in a state that is ready to dispatch, an Event object is signaled
 * which the main application can use to be woken up.
 *
 * C++ exceptions are used throughout.  Python exceptions that occur
 * from the Python API are converted to corresponding C++ exceptions
 * from the PyCcp.h header, such as PyError.  Win32 errors are also
 * raised as exceptions.  This simplifies all control flow.  Clients
 * of StacklessIO must be careful to catch all exceptions and convert
 * them back to Python exceptions as appropriate.  Utility functions to
 * do this exist in PyCcp.h, but it requires RTTI to be enabled.
 *
 * This is the core of StacklessIO: Events and the Event Queue.
 * This is then used by other files as a building block to provide
 * stackless-blocking socket operations using Windows IO Completion and
 * stackless-blocking file operations.  It is a design principle of
 * StacklessIO that its basic scheduling model is decoupled in an
 * object-oriented way from the particulars of each IO application.
 *
 ****************************************************************************/

#ifndef STACKLESSIO_H
#define STACKLESSIO_H

#ifdef _WIN32
// We must be careful here with the order of includes.  Because some projects
// have non-default struct packing, we must still use the default one when
// talking to the windows api.  So, we must manually include windows early.

#ifdef _WIN64
#pragma pack( push, 16 )
#else
#pragma pack( push, 8 )
#endif
#include <windows.h>
#pragma pack( pop )
#endif

#include <BluePyCpp.h>
#include <BluePlatform.h>

#include <Python.h>
#include <stackless_api.h>

#include <deque>
#include <vector>

#if defined _WIN32
#include <unordered_map>
#include <unordered_set>
#elif defined __APPLE__
// remove macros from pyport.h.  This will be fixed in future 2.7 headers
#undef tolower
#undef toupper
#undef isspace
#undef islower
#undef isalpha
#undef isalnum
#undef isupper
#include <ext/hash_map>
#include <ext/hash_set>
// Add string hashing
namespace __gnu_cxx
{
template <>
struct hash<std::string>
{
	size_t operator()( const std::string& s ) const
	{
		return hash<const char*>()( s.c_str() );
	}
};
}
#endif

// A simple smartpointer to manage references to IOEvents and
// related objects.  Based on boost intrusive_ptr.
// We use this rather than e.g. std::shared_ptr for two reasons:
// 1) this is simpler, and it is easier to fixed references when
//    handing objects off to threads.
// 2) It avoids having to add sdt::shared_ptr compatibility
// (e.g. enabled_shared_from_this) from the DLL interface.
template <class T>
class IOPtr
{
public:
	IOPtr() throw() :
		p( 0 )
	{
	}

	IOPtr( T* _p, bool add_ref = true ) :
		p( _p )
	{
		if( add_ref )
			AddRef();
	}

	IOPtr( IOPtr const& o ) throw() :
		p( o.p )
	{
		AddRef();
	}

	template <class C>
	IOPtr( IOPtr<C> const& o ) throw() :
		p( static_cast<T*>( o.get() ) )
	{
		AddRef();
	}

	~IOPtr() throw()
	{
		try
		{
			Release();
		}
		catch( ... )
		{
			/* just swallow all exceptions here */
		}
	}

	IOPtr& operator=( IOPtr const& o ) throw()
	{
		IOPtr( o ).swap( *this );
		return *this;
	}

	template <class C>
	IOPtr& operator=( IOPtr<C> const& o ) throw()
	{
		*this = IOPtr( o );
		return *this;
	}

	void swap( IOPtr& o ) throw()
	{
		T* tmp = p;
		p = o.p;
		o.p = tmp;
	}

	T* detach() throw()
	{
		T* tmp = p;
		p = 0;
		return tmp;
	}

	void reset() throw()
	{
		IOPtr().swap( *this );
	}

	void reset( T* p ) throw()
	{
		IOPtr( p ).swap( *this );
	}

	T& operator*() const throw()
	{
		_ASSERT( p );
		return *p;
	}

	T* operator->() const throw()
	{
		_ASSERT( p );
		return p;
	}

	T** operator&() throw()
	{
		_ASSERT( p == NULL );
		return &p;
	}

	operator bool() const throw()
	{
		return !!p;
	}

	T* get() const throw()
	{
		return p;
	}

private:
	void AddRef()
	{
		if( p )
			IOPtr_AddRef( p );
	}
	void Release()
	{
		if( p )
			IOPtr_Release( p );
	}

	T* p;
};


// Virtual interface for async stuff and timers
class IIORuntimeSvc
{
public:
	typedef void ( *callback_t )( void* arg );
	typedef void* handle_t;
	static handle_t InvalidHandle()
	{
		return (handle_t)-1;
	}

	// Schedule a callback for later and return the callback handle
	virtual handle_t CreateCallLater( callback_t cb, void* arg, double delay ) = 0;

	// Destroy a callback.  If 'sync' is true, when the function returns the callback
	// either has completed, or will never run.  If 'sync' is false, it may return before
	// the function has run to its completion.
	virtual void DeleteCallLater( handle_t handle, bool sync = true ) = 0;

	// Schedule a callback for async execution on a thread
	virtual void CallAsync( callback_t cb, void* arg ) = 0;
};


// Definition of the async stuf for mac and windows
#if defined _WIN32
class IORuntimeSvc : public IIORuntimeSvc
{
public:
	handle_t CreateCallLater( callback_t cb, void* arg, double delay );
	void DeleteCallLater( handle_t handle, bool sync = true );
	void CallAsync( callback_t cb, void* arg );

private:
	struct cb_struct
	{
		cb_struct( callback_t _cb, void* _arg ) :
			cb( _cb ), arg( _arg )
		{
		}
		callback_t cb;
		void* arg;
	};
	static VOID CALLBACK TimerCallback( PVOID lpParameter, BOOLEAN TimerOrWaitFired );
	static DWORD WINAPI WorkCallback( LPVOID lpParameter );
	Ccp::Mutex mut;
};
#elif defined __APPLE__
class IORuntimeSvc : public IIORuntimeSvc
{
public:
	handle_t CreateCallLater( callback_t cb, void* arg, double delay );
	void DeleteCallLater( handle_t handle, bool sync = true );
	void CallAsync( callback_t cb, void* arg );

private:
	static void TimerCallback( void* arg );
	Ccp::Mutex mut;
};
#endif



// Forward decleration of the IOEvent class
PyAPI_CLASS( IOEvent );

// typedef of the smart pointer we use
typedef IOPtr<IOEvent> IOEventPtr_t;

// The EventQueue Class.  This handles three things
// 1) Submitting and dispatching events.  Events submit themselves to the
//    queue and external entities then perform the dispatching at convenient
//    points with the GIL held.
// 2) The dustbin.  Dead events are put there and get their final deletion
//    with the GIL held.  This allows object destructors to decref
//    Python objects that need to have the GIL held.
// 3) The management of statistics.  We keep track of various timings
//    and counts of events.  These are for internal diagnostics.
class IOEventQueue
	: public IORuntimeSvc
{
	friend class IOEvent;

public:
	IOEventQueue();
	~IOEventQueue();

	/// The client interface
	// Get the singleton
	static IOEventQueue& GetSingleton();

	// Submit an event to the queue for later dispatch.  This is typically
	// called from the Event object itself.  Threadsafe.
	void SubmitEvent( IOEvent* r );

	// Dispatch submitted events.  This should be called with the GIL held
	// and will call the Dispatch virtual function for all the events in the
	// queue in order, or until one of them requests not to be dispatched.
	// It is typically called from the main loop, but can also be called
	// from other convenient places to facilitate throughput, such as when
	// an event is about to go to sleep waiting for IO.
	// The "from" parameter is a character string that is used for statistics
	// and is copied.
	// Returns the number of active dispatches performed.
	int Dispatch( const char* from );

#ifdef _WIN32
	// Get the Wakeup handle that the Queue maintains.  This is an Automatic
	// reset event that is signaled whenever a new event enters the queue.
	// This allows an application to break its "sleep" phase when IO is complete.
	HANDLE GetWakeupEventHandle();
#endif

	// Wait until io is ready, at most "time" seconds.
	// Returns true if IO is ready, else false.
	// Throws Ccp::Win32Error on failure.
	bool Wait( double time );

	//If the thread is currently Waiting in the Wait() call, this can be called from
	//another thread to interrupt the wait.
	// Throws Ccp::Win32Error on failure.
	void BreakWait();

	// Get the status of the queue
	struct Status
	{
		int nNonRunnable; //events that need "ticking" in the queue
		int nRunnable; //ticked events,
	};
	void GetStatus( Status& );

	// dustbin interface
protected:
	// submit for deletion.  Final destruction of requests  must happen on
	// with the GIL held since Event objects are typically allocated
	// using the python allocator and may destruct other python objects.
	void DustbinSubmit( IOEvent* r );

	// internal interface for module functions, etc
public:
	// The following update timers, stats, events.  The timers must be
	// fixed pointers as they are not copied.  The InternTimerString can
	// be used to intern a dynamic name into a constant one.
	void IncrementTimer( const char* name, Ccp::performance_t time );
	void IncrementStat( const char* name, float value );
	void IncrementEvent( const char* name, int count = 1 );
	const char* InternTimerString( const std::string& );

	// Returns a Python dictionary with the stats.
	PyObject* GetStats();
	void ClearStats();

	//Settings accessors
	bool GetUsePendingCalls() const
	{
		return mUsePendingCalls;
	}
	void SetUsePendingCalls( bool v )
	{
		mUsePendingCalls = v;
	}
	bool GetUseThreadDispatch() const
	{
		return mUseThreadDispatch;
	}
	void SetUseThreadDispatch( bool v )
	{
		mUseThreadDispatch = v;
	}
	bool GetUseOptionalThreadDispatch() const
	{
		return mUseOptionalThreadDispatch;
	}
	void SetUseOptionalThreadDispatch( bool v )
	{
		mUseOptionalThreadDispatch = v;
	}


	// Handle unraisable standard errors in a safe manner. Threadsafe.
	static void WriteUnraisable( const std::exception& e, const char* msg = 0 );

	static void WriteUnraisable( const char* msg = 0 );

	//Handle unraisable python exceptions in a safe manner
	static void PyWriteUnraisable( const char* msg );

	// Internal methods
private:
	// Internal dispatching method
	int Dispatch_impl( const char* from );

	// Clearing the dustbin
	void DustbinClear();

	// The "pending calls" dispatching feature.
	void SchedulePendingCall();
	static int Py_PendingCall_Thunk( void* arg );
	int Py_PendingCall();

	// The "thread" dispatching feature
	bool AttemptOptionalThreadDispatch();
	bool AttemptThreadDispatch();
	void PerformThreadDispatch();

	// Wake up the application by signaling an event, possibly
	// triggering a dispatch.
	void TriggerTick();

	// The data members
private:
	mutable Ccp::Mutex mQueueCS; // Guards mQueue
	std::deque<IOEventPtr_t> mQueue; // The event queue

	// Dustbin
	mutable Ccp::Mutex mDustbinCS;
	std::vector<IOEvent*> mDustbin;

	// The application wakeup event
#ifdef _WIN32
	HANDLE mWakeupEvent; // To wake up applications.
#else
	Ccp::Condition mWakeupCond;
#endif

	bool mBreakWait; // Used when the user breaks

	//timers.  Each String has an associated "Timer" object that contains
	//counts and values.
	struct Timer
	{
		Timer()
		{
			memset( this, 0, sizeof( *this ) );
		}
		int n;
		char type;
		union
		{
			struct
			{
				Ccp::performance_t t;
				Ccp::performance_t ts; //smoothed time
				float vs; //smoothed variance
				float vvs; //smoothed third moment
			} time;
			struct
			{
				float t; //total
				float ts; //smoothed time
				float vs; //smoothed variance
				float vvs; //smoothed third moment
			} stat;
		};
	};
	Ccp::Mutex mTimerCS; // Guarding the timers
#ifdef _WIN32
	typedef const void* map_key_t;
	typedef std::unordered_map<map_key_t, Timer> timermap_t;
	typedef std::unordered_set<std::string> timerset_t;
#else
	typedef long map_key_t;
	typedef __gnu_cxx::hash_map<map_key_t, Timer> timermap_t;
	typedef __gnu_cxx::hash_set<std::string> timerset_t;
#endif
	timermap_t mTimers; // The timers
	timerset_t mTimerStrings; // Interned timer keys.

	// Synchronization primitives for the PendingCalls and
	// ThreadDispatch dispatching feature.
	Ccp::Atomic32 mPendingCalls;
	Ccp::Atomic32 mThreadDispatch;

	// Options to use PendingCalls and ThreadDispatch
	bool mUsePendingCalls;
	bool mUseThreadDispatch;
	bool mUseOptionalThreadDispatch;
};


// Define the ChannelObject pointer
typedef Ccp::PyGenericObjectPtr<PyChannelObject> PyChannelObjectPtr;


// This is the base Event class.
// An IO operation typically instantiates creates a subclass of this,
// and hands it off to a worker thread.  The worker then may submit it
// to the to the IOEventQueue for dispatching when it is done.
// We use a smart pointer to manage object lifetime since various race
// situations can come up when multiple threads own pointers to the
// same object.
// Final deletion of the IOEvent is done by the IOEventQueue, regardless
// of the object being submitted to the queue or not. This is to ensure
// that it is performed on the main python thread or within the context
// of the GIL since deleting it may involve releasing python objects.
// A special "dustbin" facility of the IOEventQueue takes care of this.
//
// IOEvents inherit from Ccp::PyMalloc so that they use the python memory
// allocator for speed and performance.  This requires allocation and
// deletion top happen with the GIL held, another reason to use the
// aforementioned "dustbin" mechanism.  This can of course be overridden
// by child classes.
#ifndef __APPLE__
namespace Ccp
{
PyAPI_CLASS( Atomic32 );
};
#endif

class BLUEIMPORT IOEvent : public Ccp::PyMalloc
{
	// The Event Queue can call protected methods on us.
	friend class IOEventQueue;


public:
	// Constructor and destructor.  Destructor is virtual, so that meaningful
	// delete can be called on the base class.
	IOEvent();
	virtual ~IOEvent();

	// Lifetime management

	// support for reference counting.  Uses the COM conventional methods names.
	// When the internal reference count goes to zero, first PreDelete is called,
	// Then the object is submitted to the EventQueue's Dustbin for deletion by
	// a GIL locked thread.
	int AddRef();
	int Release();

	// Call this prior to starting off a worker thread that will be passed
	// a plain pointer to this object.  It will create the necessary extra
	// reference.
	void PrepareHandoff();

	// Finish the handoff.  The caller now owns the reference to the returned
	// object.  The worker thread calls this to get its own reference to the thread.
	// If spawning the worker thread fails, the caller should call this to
	// undo the call to PrepareHandoff, and then release the reference.
	IOEvent* CompleteHandoff();

protected:
	// Called when reference counts goes down to zero.  The default implementation
	// calls PreDelete() and then submits the event to the EventQueue's
	// dustbin.  Subclasses can override this behaviour.
	virtual void RefZero();

	// Called by the default RefZero method.  This can be used to clear early
	// those members that are thread safe.
	virtual void PreDelete();

	// Event dispatching

protected:
	// A subclass should call this method when its work is done, to submit it
	// to the EventQueue if it desires.  It is thread safe and can be called
	// without the GIL.
	void SubmitToQueue();

	// These two functions are called by the EventQueue with the GIL held:

	// Called to query if this Event should be dispatched.  If it returns false,
	// then dispatching is halted at this point in the queue.  This is useful if
	// dispatching from the event object itself.  The default implementation returns
	// true.
	virtual bool QueueCanDispatch();

	// The virtual dispatch function.  Here, an Event implementation must do
	// its job, such as wake up a sleeping tasklet and hand over the result.
	// returns 1 if it caused an effect.
	virtual int QueueDispatch() = 0; //dispatch this event

private:
	Ccp::Atomic32 mRefcount; //reference count for lifetime management
protected:
	Ccp::performance_t mSubmitTime; //Time of request submission
};

// Support functions for IOPtr.  They delegate directly to AddRef and Release.
inline void IOPtr_AddRef( IOEvent* res )
{
	res->AddRef();
}
inline void IOPtr_Release( IOEvent* res )
{
	res->Release();
}

// A RAII helper class to hand off references to worker threads.
// Since C++ doesn't allow detecting in a destructor if it
// is being called as part of exception stack unwinding, we
// must rely on the user to signal success.
// The pattern is:
// {
//   IOHandoffHelper helper(request);
//   StartWork(request); //assumed to throw exception if it ifails
//   helper.Success();
// }
class IOHandoffHelper
{
public:
	explicit IOHandoffHelper( IOEvent* );
	~IOHandoffHelper();

	// Call this if handoff was successful (the thread _will_ call CompleteHandoff)
	void Success();

	// Call this manually if handoff was failed, in case you can't rely on the
	// destructor
	void Fail();

	// And a template function to get the proper smart pointer
	template <class T>
	static inline IOPtr<T> Complete( T* obj )
	{
		return IOPtr<T>( static_cast<T*>( obj->CompleteHandoff() ), false );
	}

private:
	IOEvent* mPtr;
};


// A virtual base class to support timeouts in IOEvents
class BLUEIMPORT IOTimeout
{
public:
	// Constructor and destructor.  The destructor will call CancelTimeout
	// to ensure that no callback will occur on a destroyed object.
	IOTimeout();
	~IOTimeout() throw();

	// Start the timeout period.  After "seconds" s, OnTimeout will be called.
	// Do not call more than once without calling CancelTimeout in between.
	void SetTimeout( double seconds );

	// Cancel the timeout.  After this called, either OnTimeout has completed
	// or it will never be called.
	// It is safe to call even if SetTimeout hasn't been called prior.
	void CancelTimeout();

	// The user-supplied timeout function.
	virtual void OnTimeout() = 0;

private:
	static void TimerCallback( void* arg );
	IORuntimeSvc::handle_t mTimer; // The timer handle, initially
};


// some IOResult specializations.  Clients can subclass these further or roll their
// own as they like.


// IORequest: This subclass has a channel object, and is for requests that
// the client wants to wait for.  This is a good base class for
// Stackless events.
class BLUEIMPORT IORequest : public IOEvent, public IOTimeout
{
public:
	IORequest();

	// Submit event.  If "SetNoWait" hasn't been called, it
	// will be submitted to the Event Queue.
	void SubmitToQueue();

	// ensure dispatch termination if dispatch came from ourselves.
	bool QueueCanDispatch();

	// The initiator calls this.  This will cause a wait on the internal
	// channel.  The tasklet will be suspended and made runnable once
	// the IO has completed.  This is to guarantee fairness in the system.
	// Unless SetNoWait() has been called in which case this call does
	// nothing.
	void WaitForCompletion();

	// Reset the wait state.  Call this if already waited and signaled,
	// if we expect to be signaled again.
	void WaitReset();

	// Optionally, call this function prior to submitting.  It will mark
	// this request as "fire and forget".
	// This causes submission to the queue to be skipped, and
	// WaitForCompletion to return immediately.
	// This is an optimization only since it is safe to dispatch an
	// event that no one is waiting for, i.e. the alternative is to simply
	// not call WaitForCompletion.
	void SetNoWait()
	{
		mNoQueue = true;
	}
	bool GetNoWait() const
	{
		return mNoQueue;
	}

	// Timeout support
	void SetTimeout( double seconds );
	void OnTimeout();
	bool OnNonTimeout();
	bool IsTimeout() const
	{
		return mTimedOut.value() == 1;
	}

protected:
	// Called by the Queue when the event is dispatched.
	// If a tasklet is waiting for the channel, it is woken up.
	virtual int QueueDispatch();

private:
	PyChannelObjectPtr mChannel; //the channel on which someone may be waiting.
	bool mBusy; //used when dispatching early
	bool mNoQueue; // If true, don't put this in the queue, no one will wait.
	Ccp::Atomic32 mTimedOut; // Flagged to 1 if request ran out
};


// Specialization of the IORequest that performs a task on a worker thread.
// This uses a thread from the system thread pool.
class BLUEIMPORT IOWorker : public IORequest
{
public:
	// Start the worker thread on this object and wait for it to finish,
	// unless SetNoWait() was called, in which case it returns right away.
	void ExecuteAndWait();

private:
	// Redefine this in a class to speficy if a request will take a long
	// time to complete.
	virtual bool IsExecuteLong() const
	{
		return false;
	}

	// The function that defines the work done by the thread. When this function
	// is done, the IOWorker is submitted to the IO queue.
	// The function must handle all exceptions itself.
	virtual void ThreadFunc() = 0;

	// Thread start thunker
	static void ThreadProc( void* lpParameter );
};


#ifdef _WIN32
// A specialization of IORequest to work with IO Completion Ports and
// Overlapped IO.
// Here is how it works: A static method, Bind, allows the user to bind
// a supplied HANDLE and the IOOverlapped's IO handling method to the
// IO Completion Port serviced by the system thread pool.
// The user can then initiate IO using the IORequest's Overlapped()
// method, which exposes the OVERLAPPED member.
// Once IO is completed, a thread from the thread pool
// handles it and, through the bound static callback handler, invokes
// HandleCompletion function that must be defined by a subclass.
class BLUEIMPORT IOOverlapped : public IORequest
{
public:
	IOOverlapped() :
		mErrorCode( 0 ), mNumberOfBytesTransfered( 0 )
	{
		memset( &overlapped, 0, sizeof( overlapped ) );
	}

	// Bind a handle to the iocompletion routine serviced by this
	// base class.  This causes all overlapped io for this handle
	// to assume that it invoked using an IORequest.
	static void Bind( HANDLE h );

	//Get the overlapped pointer, use when calling the IO
	//routines.
	OVERLAPPED* Overlapped()
	{
		return &overlapped;
	}

	//Get the overlapped pointer with a specific position
	void SetPos( unsigned __int64 pos )
	{
		ULARGE_INTEGER li;
		li.QuadPart = pos;
		overlapped.Offset = li.LowPart;
		overlapped.OffsetHigh = li.HighPart;
	}

	//A handler function.  The default impl just calls SubmitToQueue()
	virtual void HandleCompletion();

	//accessor
	DWORD GetErrorCode() const
	{
		return mErrorCode;
	}
	DWORD GetBytesTransfered() const
	{
		return mNumberOfBytesTransfered;
	}
	void SetErrorCode( DWORD code )
	{
		mErrorCode = code;
	}
	void SetBytesTransfered( DWORD b )
	{
		mNumberOfBytesTransfered = b;
	}

private:
	// Static IO Completion thunker.  Recovers the IOOverlapped pointer
	// from the lpOverlapped pointer and invokes HandleCompletion.
	static void CALLBACK FileIOCompletionRoutine(
		DWORD dwErrorCode,
		DWORD dwNumberOfBytesTransfered,
		LPOVERLAPPED lpOverlapped );

private:
	OVERLAPPED overlapped;
	DWORD mErrorCode; //result
	DWORD mNumberOfBytesTransfered;
};

#endif


// DLL export routines to deal with the module.
// These are deprecated since they are not extern "C".
BLUEIMPORT void PyStacklessIoSubmit( IOEvent* r );
BLUEIMPORT int PyStacklessIoDispatch( const char* from );

// see also stacklessioapi.h

#endif // STACKLESSIO_H
