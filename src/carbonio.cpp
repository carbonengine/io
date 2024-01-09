/****************************************************************************
 * stacklessio.cpp
 * This file contains the implementation for the classes declared in
 * stacklessio.h
 *
 * It also contains the stacklessio python module definition and definitions
 * for module functions.
 ****************************************************************************/
#include <carbonio.h>
#include "stacklessio_api.h"
#include <protocol.h>
#include <BluePyCpp.h>

#ifdef __APPLE__
#include <dispatch/dispatch.h> // for timeouts and async calls
#endif

#include <algorithm>

using Ccp::PyObjectPtr;
using Ccp::MutexOwner;
using Ccp::CriticalSectionOwner;

//The global IOResult queue.  This is currently a static.
static IOEventQueue PyStacklessIOEventQueue;

static bool s_wakeupEventActive = true;


/////////////////////////////////////////////////
// IORuntimeSvc code
#if defined _WIN32
IORuntimeSvc::handle_t
	IORuntimeSvc::CreateCallLater( callback_t cb, void* arg, double delay )
{
	HANDLE h;
	cb_struct* s = new cb_struct( cb, arg );
	DWORD DueTime = (DWORD)( delay * 1000.0 ); // ms
	ULONG Flags = WT_EXECUTEDEFAULT | WT_EXECUTEONLYONCE; // WT_EXECUTELONGFUNCTION
	BOOL ok = CreateTimerQueueTimer( &h, NULL, TimerCallback, (PVOID)s, DueTime, 0, Flags );
	if( !ok )
	{
		delete s;
		throw Ccp::SystemError( "CreateTimerQueueTimer" );
	}
	return (handle_t)h;
}

void IORuntimeSvc::DeleteCallLater( handle_t handle, bool sync )
{
	HANDLE h = (HANDLE)handle;
	HANDLE CompletionEvent = sync ? INVALID_HANDLE_VALUE : NULL;
	BOOL ok = DeleteTimerQueueTimer( NULL, h, CompletionEvent );
	if( !ok )
		throw Ccp::SystemError( "DeleteTimerQueueTimer" );
}

void IORuntimeSvc::CallAsync( callback_t cb, void* arg )
{
	cb_struct* s = new cb_struct( cb, arg );
	ULONG Flags = WT_EXECUTEDEFAULT; // WT_EXECUTELONGFUNCTION;
	BOOL ok = QueueUserWorkItem( WorkCallback, (PVOID)s, Flags );
	if( !ok )
	{
		delete s;
		throw Ccp::SystemError( "QueueUserWorkItem" );
	}
}

VOID CALLBACK IORuntimeSvc::TimerCallback( PVOID lpParameter, BOOLEAN TimerOrWaitFired )
{
	(void)TimerOrWaitFired;
	cb_struct* ps = (cb_struct*)lpParameter;
	cb_struct s = *ps;
	delete ps;
	s.cb( s.arg );
}

DWORD WINAPI IORuntimeSvc::WorkCallback( LPVOID lpParameter )
{
	cb_struct* ps = (cb_struct*)lpParameter;
	cb_struct s = *ps;
	delete ps;
	s.cb( s.arg );
	return 0;
}

#elif defined __APPLE__

struct IORuntimeCallLater_t
{
	IORuntimeCallLater_t( IORuntimeSvc& _svc, IORuntimeSvc::callback_t _cb, void* _arg ) :
		svc( _svc ), cb( _cb ), arg( _arg ), refcount( 2 ), should_run( true )
	{
	}

	void Release()
	{
		if( refcount.Decrement() == 0 )
			delete this;
	}

	IORuntimeSvc& svc;
	IORuntimeSvc::callback_t cb;
	void* arg;
	Ccp::Atomic32 refcount;
	bool should_run;
};

void IORuntimeSvc::TimerCallback( void* arg )
{
	IORuntimeCallLater_t* t = reinterpret_cast<IORuntimeCallLater_t*>( arg );
	Ccp::MutexOwner own( t->svc.mut );
	if( t->should_run )
		t->cb( t->arg );
	t->Release();
}

IORuntimeSvc::handle_t
	IORuntimeSvc::CreateCallLater( callback_t cb, void* arg, double delay )
{
	IORuntimeCallLater_t* t = new IORuntimeCallLater_t( *this, cb, arg );

	dispatch_queue_t queue = dispatch_get_global_queue( DISPATCH_QUEUE_PRIORITY_DEFAULT, 0 );
	dispatch_time_t when = dispatch_time( DISPATCH_TIME_NOW, (int64_t)( delay * 1e9 ) );
	dispatch_after_f( when, queue, (void*)t, &TimerCallback );
	return (handle_t)t;
}

void IORuntimeSvc::DeleteCallLater( handle_t handle, bool sync )
{
	IORuntimeCallLater_t* t = (IORuntimeCallLater_t*)handle;
	if( sync )
	{
		Ccp::MutexOwner own( mut );
		t->should_run = false;
	}
	else
	{
		t->should_run = false;
	}
	t->Release();
}

void IORuntimeSvc::CallAsync( callback_t cb, void* arg )
{
	dispatch_queue_t queue = dispatch_get_global_queue( DISPATCH_QUEUE_PRIORITY_DEFAULT, 0 );
	dispatch_async_f( queue, arg, cb );
}

#endif // __APPLE__



/////////////////////////////////////////////////
// IOEvent definitions

IOEvent::IOEvent() :
	mRefcount( 0 ), mSubmitTime( 0 )
{
}

IOEvent::~IOEvent()
{
}

int IOEvent::AddRef()
{
	return mRefcount.Increment();
}

int IOEvent::Release()
{
	int r = mRefcount.Decrement();
	if( !r )
		RefZero();
	return r;
}

void IOEvent::PrepareHandoff()
{
	AddRef();
}

IOEvent* IOEvent::CompleteHandoff()
{
	return this;
}

// A subclass can use this to delete things early
void IOEvent::PreDelete()
{
}

// The default implementaiton executes PreDelete
// and then enqueues it for later deletion by a
// GIL thread.
void IOEvent::RefZero()
{
	// Perform early cleanup
	PreDelete();
	// submit for later deletion on main thread
	PyStacklessIOEventQueue.DustbinSubmit( this );
}

void IOEvent::SubmitToQueue()
{
	if( !mSubmitTime )
		mSubmitTime = Ccp::GetPerformanceTime();
	PyStacklessIOEventQueue.SubmitEvent( this );
}

bool IOEvent::QueueCanDispatch()
{
	return true; //default implementation returns true
}


/////////////////////////////////////////////////
// The IOHandoffHelper class

IOHandoffHelper::IOHandoffHelper( IOEvent* e ) :
	mPtr( e )
{
	_ASSERT( mPtr );
	mPtr->PrepareHandoff();
}

IOHandoffHelper::~IOHandoffHelper()
{
	// If Success() wasn't called, we assume handoff failed.
	Fail();
}

void IOHandoffHelper::Success()
{
	mPtr = NULL;
}

void IOHandoffHelper::Fail()
{
	if( mPtr )
	{
		// Take the smart pointer and let it go
		Complete( mPtr );
		mPtr = 0;
	}
}


/////////////////////////////////////////////////
// The IOTimout class, adding timeout support.


IOTimeout::IOTimeout() : // Todo, enable timers on posix
	mTimer( IORuntimeSvc::InvalidHandle() )
{
}


IOTimeout::~IOTimeout() throw()
{
	try
	{
		CancelTimeout();
	}
	catch( ... )
	{
	}
}


void IOTimeout::SetTimeout( double seconds )
{
	_ASSERT( mTimer == IORuntimeSvc::InvalidHandle() );
	mTimer = IOEventQueue::GetSingleton().CreateCallLater( TimerCallback, (void*)this, seconds );
}


void IOTimeout::CancelTimeout()
{
	if( mTimer != IORuntimeSvc::InvalidHandle() )
	{
		// The following guarantees atomicity.  Once completed, either the
		// timeout function will never be called, or it has been called and
		// completed.
		IOEventQueue::GetSingleton().DeleteCallLater( mTimer );
		mTimer = IORuntimeSvc::InvalidHandle();
	}
}


void IOTimeout::TimerCallback( void* lpParameter )
{
	IOTimeout* self = reinterpret_cast<IOTimeout*>( lpParameter );
	self->OnTimeout();
}


/////////////////////////////////////////////////
// The IORequest class, an IOEvent with a channel


IORequest::IORequest() :
	mBusy( false ),
	mNoQueue( false ),
	mTimedOut( 0 )
{
}


void IORequest::SubmitToQueue()
{
	if( GetNoWait() )
		return; //This one is not to be submitted, just forgotten.
	IOEvent::SubmitToQueue(); //submit now, so that main thread has access
}

// Set the timeout, resetting any timeout status
void IORequest::SetTimeout( double delay )
{
	CancelTimeout();
	mTimedOut = 0;
	if( delay >= 0.0 )
		IOTimeout::SetTimeout( delay );
}

//If a timeout occurred, we submit to queue as well.
void IORequest::OnTimeout()
{
	if( mTimedOut.CompareExchange( 0, 1 ) )
		SubmitToQueue();
}

//Signal that we didn't timeout.  Returns true if successful,
//false in case we lost the race with he timeout timer.
bool IORequest::OnNonTimeout()
{
	if( mTimedOut.CompareExchange( 0, -1 ) )
		return true;
	return mTimedOut.value() == -1;
}

void IORequest::WaitForCompletion()
{
	if( GetNoWait() )
	{
		//We are not waiting for this request.  Just increment a stat thing.
		PyStacklessIOEventQueue.IncrementEvent( "request::FireAndForget" );
		return;
	}
	// It is possible that the request is already complete.  This can happen
	// if it was created earlier, by a different tasklet or on a different
	// thread.  This is indicated by the presence of mChannel, which otherwis
	// would be created by us a bit lower down.  We use this member as a
	// flag to preserve binary compatibility.
	if( mChannel )
	{
		PyStacklessIOEventQueue.IncrementEvent( "request::AlreadyReady" );
		return;
	}

	//dispatch a bit, upto ourselves in the queue.  Ignore any errors
	mBusy = true;
	try
	{
		PyStacklessIOEventQueue.Dispatch( "dispatch::PreWaitForCompletion" );
	}
	catch( const Ccp::PyError& e )
	{
		if( !e.Matches( PyExc_Exception ) )
		{
			mBusy = false;
			throw; // some tasklet exit error
		}
	}
	catch( ... )
	{
	}
	mBusy = false;

	//create the channel on demand.  It is important that no one has tried to dispatch
	//us yet.
	mChannel = PyChannelObjectPtr( Ccp::PyCheck( PyChannel_New( &PyChannel_Type ) ) );
	// Wait for the request to be dispatched!
	PyObjectPtr dummy( PyChannel_Receive( mChannel ) );
	if( !dummy )
	{
		PyStacklessIOEventQueue.IncrementTimer( "request::Ready::Error",
												Ccp::GetPerformanceTime() - mSubmitTime );
		throw Ccp::PyError();
	}
	//Cancel any pending timeout, guarding ourselves against race conditions.
	CancelTimeout();

	//dispatch again, making any new tasklets runnable
	try
	{
		PyStacklessIOEventQueue.Dispatch( "dispatch::PostWaitForCompletion" );
	}
	catch( const Ccp::PyError& e )
	{
		if( !e.Matches( PyExc_Exception ) )
			throw; // some tasklet exit error
	}
	catch( ... )
	{
	}

	PyStacklessIOEventQueue.IncrementTimer( "request::Ready::OK",
											Ccp::GetPerformanceTime() - mSubmitTime );
}


// prepare the object to be waited for (and signaled) again.
// Use this if there are multiple passes through the EventQueue
void IORequest::WaitReset()
{
	mChannel.Clear();
}


//make sure that we stop the dispatch queue in its tracks if dispatching
//from us, before we wake up.  This avoids recursion.
bool IORequest::QueueCanDispatch()
{
	return !mBusy;
}


//Internal method.  The Queue calls this when it is dispatching
//results that are ready.  This is for sending the ready result
//over to the receiver.
int IORequest::QueueDispatch()
{
	if( GetNoWait() )
	{
		//nobody intended to listen here
		Ccp::performance_t time = Ccp::GetPerformanceTime() - mSubmitTime;
		PyStacklessIOEventQueue.IncrementTimer( "request::wakeup::NoRecv", time );
		return 0;
	}
	if( !mChannel )
	{
		// We are finished and being dispatched before the
		// WaitForCompletion call.  This can happen if the request
		// was created and started by a different tasklet or thread
		// than that which calls WaitForCompletion.  Signal that we
		// are finished by putting PyNone into mChannel
		Ccp::performance_t time = Ccp::GetPerformanceTime() - mSubmitTime;
		PyStacklessIOEventQueue.IncrementTimer( "request::wakeup::Early", time );
		mChannel = PyChannelObjectPtr( (PyChannelObject*)Py_None, true );
		return 0;
	}

	if( !PyChannel_GetBalance( mChannel ) )
	{
		//nobody listening, they were aborted or something worse.
		Ccp::performance_t time = Ccp::GetPerformanceTime() - mSubmitTime;
		PyStacklessIOEventQueue.IncrementTimer( "request::wakeup::Zombie", time );
		return 0;
	}
	else
	{
		//make certain that we continue execution, i.e. the receiver is only made
		//runnable at this point.
		PyChannel_SetPreference( mChannel, 0 );
		if( PyChannel_Send( mChannel, Py_None ) )
			throw Ccp::PyError();
		return 1;
	}
}


/////////////////////////////////////////////////
// IOWorker methods


void IOWorker::ExecuteAndWait()
{
	IOHandoffHelper handoff( this );
	PyStacklessIOEventQueue.CallAsync( ThreadProc, reinterpret_cast<void*>( this ) );
	handoff.Success();
	WaitForCompletion();
}


void IOWorker::ThreadProc( void* lpParameter )
{
	// Complete the pointer handoff
	IOPtr<IOWorker> self = IOHandoffHelper::Complete( static_cast<IOWorker*>( lpParameter ) );
	// Handle top level errors from the thread func.
	try
	{
		self->ThreadFunc();
	}
	catch( const std::exception& e )
	{
		PyStacklessIOEventQueue.WriteUnraisable( e, "ThreadFunc" );
	}
	if( self->OnNonTimeout() )
	{
		// Event did not time out.
		// Handle top level errors from event submission.
		try
		{
			self->SubmitToQueue();
		}
		catch( const std::exception& e )
		{
			PyStacklessIOEventQueue.WriteUnraisable( e, "SubmitToQueue" );
		}
	}
}



#ifdef _WIN32

/////////////////////////////////////////////////
// The IOOverlapped class definition


//Here, bind a handle to use this mechanism.  Any IO completed on this
//handle will invoke the static class method callback.
void IOOverlapped::Bind( HANDLE h )
{
	BOOL ok = BindIoCompletionCallback( h, FileIOCompletionRoutine, 0 );
	if( !ok )
		throw Ccp::SystemError( "BindIoCompletionCallback" );
}


//The callback for the IOOverlapped result
void CALLBACK IOOverlapped::FileIOCompletionRoutine(
	DWORD dwErrorCode,
	DWORD dwNumberOfBytesTransfered,
	LPOVERLAPPED lpOverlapped )
{
	// Complete the pointer handoff
	// Use a proper macro to extract the IOOverlapped pointer.
	IOPtr<IOOverlapped> self = IOHandoffHelper::Complete(
		CONTAINING_RECORD( lpOverlapped, IOOverlapped, overlapped ) );

	self->SetErrorCode( dwErrorCode );
	self->mNumberOfBytesTransfered = dwNumberOfBytesTransfered;

	// Invoke the virtual handling function
	try
	{
		self->HandleCompletion();
	}
	catch( const std::exception& e )
	{
		PyStacklessIOEventQueue.WriteUnraisable( e, "HandleCompletion" );
	}
}


// The default handling function just submits the result to the queue.
void IOOverlapped::HandleCompletion()
{
	if( OnNonTimeout() )
		SubmitToQueue();
}

#endif


/////////////////////////////////////////////////
// The IOEventQueue implementation


IOEventQueue::IOEventQueue() :
#ifdef _WIN32
	mWakeupEvent( 0 ),
#endif
	mBreakWait( false ),
	mPendingCalls( 0 ),
	mThreadDispatch( 0 ),
	mUsePendingCalls( false ),
	mUseThreadDispatch( false ),
	mUseOptionalThreadDispatch( false )
{
#ifdef _WIN32
	//Create an automatic event, initially not set
	mWakeupEvent = CreateEvent( 0, FALSE, FALSE, 0 );
	if( !mWakeupEvent )
		throw Ccp::SystemError( "CreateEvent" );
#endif
	ClearStats();
}


IOEventQueue::~IOEventQueue()
{
	//yes, we may get destroyed while there are still threads
	//in existence!  Try this band aid (the full solution is to reference count this)
	CriticalSectionOwner own( mQueueCS );
	mQueue.clear();
	//Cannot clear Dustbin at this point, since its contents was allocated
	//using Python malloc.  And pyton may have shut down at this point and
	//no GIL available.  Allow those objects to stick around.
	//DustbinClear();
#ifdef _WIN32
	if( mWakeupEvent )
		CloseHandle( mWakeupEvent );
#endif
}


IOEventQueue& IOEventQueue::GetSingleton()
{
	return PyStacklessIOEventQueue;
}


void IOEventQueue::SubmitEvent( IOEvent* r )
{
	{
		CriticalSectionOwner own( mQueueCS );
		mQueue.push_back( IOEventPtr_t( r ) );
	}
	TriggerTick();
}



//Convenience function: Wait until io is ready in the queue
#ifdef _WIN32
bool IOEventQueue::Wait( double time )
{
	DWORD r = WAIT_FAILED;
	double waittime = time;
	__int64 starttime, freq = 0;
	QueryPerformanceCounter( (LARGE_INTEGER*)&starttime );
	while( waittime > 0.0 )
	{
		{
			Ccp::PyAllowThreads _allow;
			int ms = (int)( waittime * 1000.0 );
			if( ms <= 0 )
				ms = 1; //always wait the minimum amount possible
			r = WaitForMultipleObjects( 1, &mWakeupEvent, FALSE, ms );
		}
		if( mBreakWait )
			break; //someone called BreakWait
		if( r == WAIT_OBJECT_0 )
		{
			//Guard ourselves from unnecessary wakeups.
			Status s;
			GetStatus( s );
			if( s.nNonRunnable || s.nRunnable )
			{
				waittime = 0.0; //there is work to do
			}
			else
			{
				//nothing to do, readjust the wait time.
				__int64 now;
				QueryPerformanceCounter( (LARGE_INTEGER*)&now );
				if( !freq )
					QueryPerformanceFrequency( (LARGE_INTEGER*)&freq );
				waittime = time - (double)( now - starttime ) / (double)freq;
			}
		}
		else if( r == WAIT_FAILED )
			throw Ccp::SystemError( "WaitForMultipleObjects" );
		else
			break; //timeout
	}

	// There are a number of race conditions possible with the mBreakWait
	// member, for example, someone may just have called BreakWait when we
	// clear it here.  But it is not considered a "guaranteed" function and
	// its semantics are somewhat vague.  A more robust way would be to have
	// a separate Event object for it.  But let's not worry about it.
	mBreakWait = false;
	return r == WAIT_OBJECT_0;
}


//Break any Wait that currently is in progress
void IOEventQueue::BreakWait()
{
	mBreakWait = true;
	if( !::SetEvent( mWakeupEvent ) )
		throw Ccp::SystemError( "SetEvent" );
}

#else

bool IOEventQueue::Wait( double time )
{
	Ccp::Condition::deadline_t deadline = Ccp::Condition::ComputeDeadline( time );
	MutexOwner own( mQueueCS );
	for( ;; )
	{
		if( mBreakWait )
			break; //someone called BreakWait
		Status s;
		GetStatus( s );
		if( s.nNonRunnable || s.nRunnable )
			break;
		if( !mWakeupCond.TimedWait( mQueueCS, deadline ) )
			return false;
	}
	mBreakWait = false;
	return true;
}

//Break any Wait that currently is in progress
void IOEventQueue::BreakWait()
{
	MutexOwner own( mQueueCS );
	mBreakWait = true;
	mWakeupCond.Broadcast();
}

#endif

void IOEventQueue::GetStatus( Status& s )
{
	s.nNonRunnable = (int)mQueue.size();
	//Instead of maintaining complex bookkeeping on the wakeups of sleeping tasklets,
	//including the possibility that a waiting tasklet may wake up not by a tick but by
	//an external influence, we just use this here.  A shortcut, ifyou will.
	s.nRunnable = PyStackless_GetRunCount() - 1;
}


// Dispatching routines

// Dispatch and empty Dustbin
int IOEventQueue::Dispatch( const char* from )
{
	int result = 0;
	if( mQueue.size() )
		result = Dispatch_impl( from ); //warning: Could possibly miss updates from a thread here.
	DustbinClear();
	return result;
}

// Actual dispatching
int IOEventQueue::Dispatch_impl( const char* from )
{
	int n = 0;
	const char* ts = 0;
	IncrementEvent( from ); //just increment counter
	for( ;; )
	{
		IOEventPtr_t ioevent;
		{
			CriticalSectionOwner own( mQueueCS );
			if( !mQueue.size() )
				return n;
			if( !mQueue.front()->QueueCanDispatch() )
				return n; //the event requested that dispatch be terminated
			ioevent = mQueue.front();
			mQueue.pop_front();
			if( !ts )
				ts = InternTimerString( std::string( "event::" ) + from );
		}
		//work timing stats
		Ccp::performance_t submitTime = ioevent->mSubmitTime;
		Ccp::performance_t now = Ccp::GetPerformanceTime();
		Ccp::performance_t diff = now - submitTime;
		IncrementTimer( ts, diff );

		if( ioevent->QueueDispatch() )
			n++;

		//Queue length stat
		// FIXME - do we still need this?
		//		IncrementStat("event::queuelen", (float)PyStackless_GetRunCountTs(slp_initial_tstate));
	}
	return n;
}

// Make sure that the queue is dispatched.  This sets the event to wake up the application
// and also kicks off a PendinCalls dispatch and a ThreadDispatch if so configured.
void IOEventQueue::TriggerTick()
{
	if( mQueue.size() )
	{
		// Opportunistic dispatching on the thread first, if we can get
		// the GIL without blocking.
		bool dispatched = AttemptOptionalThreadDispatch();
		// Only wake up the thread after our opportunistic dispatch.
		if( s_wakeupEventActive )
		{
#ifdef _WIN32
			if( mWakeupEvent )
				::SetEvent( mWakeupEvent );
#else
			Ccp::MutexOwner own( mQueueCS );
			mWakeupCond.Broadcast();
#endif
		}
		if( !dispatched )
		{
			if( mUsePendingCalls )
				SchedulePendingCall();
			AttemptThreadDispatch();
		}
	}
}

// A helper function, which takes care of error handling when creating the message
void IOEventQueue::PyWriteUnraisable( const char* msg )
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

void IOEventQueue::WriteUnraisable( const std::exception& e, const char* msg )
{
	//Must be thread safe.
	OutputDebugString( "Unraisable exception in IOEventQueue\n" );
	if( msg )
	{
		OutputDebugString( msg );
		OutputDebugString( "\n" );
	}
	OutputDebugString( e.what() );
	OutputDebugString( "\n" );
}

void IOEventQueue::WriteUnraisable( const char* msg )
{
	//Must be thread safe.
	OutputDebugString( "Unraisable exception in IOEventQueue\n" );
	if( msg )
	{
		OutputDebugString( msg );
		OutputDebugString( "\n" );
	}
}

#ifdef _WIN32
HANDLE IOEventQueue::GetWakeupEventHandle()
{
	return mWakeupEvent;
}
#endif

void IOEventQueue::IncrementTimer( const char* name, Ccp::performance_t time )
{
	Ccp::CriticalSectionOwner own( mTimerCS );
	Timer& t = mTimers[reinterpret_cast<map_key_t>( name )];
	t.n += 1;
	if( time )
	{
		t.time.t += time;
		if( t.time.ts )
		{
			float dt = (float)Ccp::PerformanceTimeToS( time - t.time.ts );
			float var = dt * dt;
			//exponential smoothing with f = 1/128
			t.time.ts = ( 127 * t.time.ts + time + 64 ) >> 7;

			/* must compute variance and skew as floats, since itegers will
			 * overrun 64 bits easily when multiplied.
			 */
			const float f = 1.0f / 128.0f;
			t.time.vs = ( 1.0f - f ) * t.time.vs + f * var;
			var *= dt;
			t.time.vvs = ( 1.0f - f ) * t.time.vvs + f * var; //third moment
		}
		else
			t.time.ts = time;
	}
}

void IOEventQueue::IncrementStat( const char* name, float value )
{
	Ccp::CriticalSectionOwner own( mTimerCS );
	Timer& t = mTimers[reinterpret_cast<map_key_t>( name )];
	t.type = 1;
	t.n++;
	t.stat.t += value;
	if( t.stat.ts )
	{
		float dt = value - t.stat.ts;
		float var = dt * dt;
		//exponential smoothing with f = 1/128
		const float f = 1.0f / 128.0f;
		t.stat.ts = ( 1.0f - f ) * t.stat.ts + f * value;
		t.stat.vs = ( 1.0f - f ) * t.stat.vs + f * var;
		var *= dt;
		t.stat.vvs = ( 1.0f - f ) * t.stat.vvs + f * var; //third moment
	}
	else
		t.stat.ts = value;
}

void IOEventQueue::IncrementEvent( const char* name, int count )
{
	Ccp::CriticalSectionOwner own( mTimerCS );
	Timer& t = mTimers[reinterpret_cast<map_key_t>( name )];
	t.n += count;
	t.type = 2;
}

const char* IOEventQueue::InternTimerString( const std::string& name )
{
	Ccp::CriticalSectionOwner own( mTimerCS );
	return mTimerStrings.insert( name ).first->c_str();
}

void IOEventQueue::ClearStats()
{
	//we don't clear the interned strings to avoid complications when
	//requests perform a timer clear.
	Ccp::CriticalSectionOwner own( mTimerCS );
	mTimers.clear();
}

PyObject* IOEventQueue::GetStats()
{
	Ccp::CriticalSectionOwner own( mTimerCS );
	PyObjectPtr list( PyList_New( mTimers.size() ) );
	if( !list )
		return 0;
	int n = 0;
	for( timermap_t::iterator i = mTimers.begin(); i != mTimers.end(); ++i )
	{
		const Timer& t = i->second;
		PyObjectPtr e;
		if( t.type == 0 )
			e = PyObjectPtr( Py_BuildValue( "siiffff",
											(const char*)( i->first ),
											t.type,
											t.n,
											Ccp::PerformanceTimeToS( t.time.t ),
											Ccp::PerformanceTimeToS( t.time.ts ),
											t.time.vs, //variance
											t.time.vvs ) ); //3rd moment
		else
			e = PyObjectPtr( Py_BuildValue( "siiffff",
											(const char*)( i->first ),
											t.type,
											t.n,
											t.stat.t,
											t.stat.ts,
											t.stat.vs, //variance
											t.stat.vvs ) ); //3rd moment
		if( !e )
			return 0;
		if( PyList_SetItem( list, n++, e.Detach() ) )
			return 0;
	}
	return list.Detach();
}


//The dustbin.  Requests are submitted to this, for final deletion on the main thread
void IOEventQueue::DustbinSubmit( IOEvent* r )
{
	// Optimization: If we have the GIL, just delete the request
	if( Ccp::PyGilHave() )
	{
		delete r;
	}
	else
	{
		CriticalSectionOwner _own( mDustbinCS );
		mDustbin.push_back( r );
	}
}

// Called by the main thread when dispatching requests.
void IOEventQueue::DustbinClear()
{
	if( mDustbin.size() )
	{
		//grab the current dustbin
		std::vector<IOEvent*> tmp;
		{
			CriticalSectionOwner _own( mDustbinCS );
			tmp.swap( mDustbin );
		}
		// Delete all members.  The destructor is virtual so the appropriate
		// delete operator will also be invoked for each object.
		for( size_t i = 0; i < tmp.size(); ++i )
			delete tmp[i];
	}
}


void IOEventQueue::SchedulePendingCall()
{
	//make sure that only one pending call is outstanding at time
	if( mPendingCalls.CompareExchange( 0, 1 ) )
	{
		//it was zero, go ahead and schedule the call, then.
		//Note, we must use a patched version of this call.  The original.
		//Python implementation is not threadsafe at all.
		int res = Py_AddPendingCall( &Py_PendingCall_Thunk, this );
		if( res == -1 )
			//queue was full.
			mPendingCalls = 0;
	}
}


int IOEventQueue::Py_PendingCall_Thunk( void* arg )
{
	IOEventQueue* self = reinterpret_cast<IOEventQueue*>( arg );
	return self->Py_PendingCall();
}


int IOEventQueue::Py_PendingCall()
{
	//Perform the pending call, which is to dispatch the stuff.
	int n = -1;
	try
	{
		n = Dispatch( "dispatch::Py_PendingCall" );
	}
	catch( const std::exception& e )
	{
		Ccp::PyErrFromException( e );
	}
	mPendingCalls = 0; //allow another to be scheduled
	return n; //returns negative on exception
}

// Thread dispatching.  We make sure that only one thread at a time
// is attempting to claim the GIL.  Others just leave the dispatching
// up to that thread.
bool IOEventQueue::AttemptThreadDispatch()
{
	if( !mUseThreadDispatch )
		return false;
	if( mThreadDispatch.CompareExchange( 0, 1 ) )
	{
		//Success, do it
		PerformThreadDispatch();
		mThreadDispatch = 0;
		return true;
	}
	return false;
}

void IOEventQueue::PerformThreadDispatch()
{
	int dispatched;
	{
		Ccp::PyGilEnsure _gil;
		try
		{
			dispatched = Dispatch( "dispatch::thread" );
		}
		catch( const std::exception& e )
		{
			Ccp::PyErrFromException( e );
			PyWriteUnraisable( "dispatch::thread" );
		}
	}
	// This may be redundant, since walready set the event when we put stuff onto the queue (so
	// that the main thread might wake up and dispatch from the queue, but anyway
	if( dispatched > 0 && s_wakeupEventActive )
	{
#ifdef _WIN32
		if( mWakeupEvent )
			::SetEvent( mWakeupEvent );
#else
		Ccp::MutexOwner own( mQueueCS );
		mWakeupCond.Broadcast();
#endif
	}
}

bool IOEventQueue::AttemptOptionalThreadDispatch()
{
	if( !mUseOptionalThreadDispatch )
		return false;

	int dispatched;
	{
		Ccp::PyGilEnsure ensure( false );
		if( !ensure.Locked() )
			return false;
		try
		{
			dispatched = Dispatch( "dispatch::othread" );
		}
		catch( const std::exception& e )
		{
			Ccp::PyErrFromException( e );
			PyWriteUnraisable( "dispatch::othread" );
		}
	}
	// This may be redundant, since walready set the event when we put stuff onto the queue (so
	// that the main thread might wake up and dispatch from the queue, but anyway
	if( dispatched > 0 && s_wakeupEventActive )
	{
#ifdef _WIN32
		if( mWakeupEvent )
			::SetEvent( mWakeupEvent );
#else
		Ccp::MutexOwner own( mQueueCS );
		mWakeupCond.Broadcast();
#endif
	}
	return true;
}

//DLL api functions to StacklessIO
void PyStacklessIoSubmit( IOEvent* r )
{
	return PyStacklessIOEventQueue.SubmitEvent( r );
}

//Deprecated C++ interface function...
int PyStacklessIoDispatch( const char* from )
{
	const char* s = PyStacklessIOEventQueue.InternTimerString( std::string( "dispatch::" ) + from );
	return PyStacklessIOEventQueue.Dispatch( s );
}

extern "C" int PyStacklessIoDispatchEvents( const char* from )
{
	const char* s = PyStacklessIOEventQueue.InternTimerString( std::string( "dispatch::" ) + from );
	try
	{
		return PyStacklessIOEventQueue.Dispatch( s );
	}
	catch( const std::exception& e )
	{
		Ccp::PyErrFromException( e );
		return -1;
	}
}

#ifdef _WIN32
extern "C" HANDLE PyStacklessIoGetWakeupEventHandle()
{
	try
	{
		return PyStacklessIOEventQueue.GetWakeupEventHandle();
	}
	catch( ... )
	{
		return INVALID_HANDLE_VALUE;
	}
}
#endif

extern "C" void
	PyStacklessIoGetStatus( PyStacklessIoStatus_t* status )
{
	try
	{
		IOEventQueue::Status s;
		PyStacklessIOEventQueue.GetStatus( s );
		if( offsetof( PyStacklessIoStatus_t, nNonRunnable ) + sizeof( status->nNonRunnable ) <= status->struct_size )
			status->nNonRunnable = s.nNonRunnable;
		if( offsetof( PyStacklessIoStatus_t, nRunnable ) + sizeof( status->nRunnable ) <= status->struct_size )
			status->nRunnable = s.nRunnable;

#undef min
		status->struct_size = std::min( status->struct_size, sizeof( PyStacklessIoStatus_t ) );
	}
	catch( ... )
	{
	}
}

//Module functions
#define PyTRY \
	try       \
	{
#define PyCATCH( v )                  \
	}                                 \
	catch( const std::exception& e )  \
	{                                 \
		Ccp::PyErrFromException( e ); \
		return ( v );                 \
	}
static PyObject* wait( PyObject* self, PyObject* args )
{
	double time = 0.0;
	if( !PyArg_ParseTuple( args, "|d:wait", &time ) )
		return 0;
	if( time < 0.0 )
	{
		PyErr_SetString( PyExc_ValueError, "time must be positive or zero" );
		return 0;
	}
	PyTRY return PyBool_FromLong( PyStacklessIOEventQueue.Wait( time ) );
	PyCATCH( NULL )
}

static PyObject* break_wait( PyObject* self )
{
	PyTRY
		PyStacklessIOEventQueue.BreakWait();
	Py_RETURN_NONE;
	PyCATCH( NULL )
}

static PyObject* dispatch( PyObject* self )
{
	(void)self;
	PyTRY
		PyStacklessIOEventQueue.Dispatch( "dispatch::pythonpoll" );
	Py_RETURN_NONE;
	PyCATCH( NULL )
}

static PyObject* GetStats( PyObject* self )
{
	(void)self;
	PyTRY return PyStacklessIOEventQueue.GetStats();
	PyCATCH( NULL )
}

static PyObject* ClearStats( PyObject* self )
{
	(void)self;
	PyTRY
		PyStacklessIOEventQueue.ClearStats();
	Py_RETURN_NONE;
	PyCATCH( NULL )
}

static PyObject* GetStatus( PyObject* self )
{
	PyTRY
		IOEventQueue::Status s;
	PyStacklessIOEventQueue.GetStatus( s );
	return Py_BuildValue( "{sisi}",
						  "nNonRunnable",
						  s.nNonRunnable,
						  "nRunnable",
						  s.nRunnable );
	PyCATCH( NULL )
}

#ifdef _WIN32
static PyObject* GetWakeupEventHandle( PyObject* self )
{
	(void)self;
	PyTRY return PyLong_FromVoidPtr( PyStacklessIOEventQueue.GetWakeupEventHandle() );
	PyCATCH( NULL )
}
#endif

static PyObject* GetSettings( PyObject* self )
{
	PyTRY return Py_BuildValue( "{sNsNsN}",
								"usePendingCalls",
								PyBool_FromLong( (int)PyStacklessIOEventQueue.GetUsePendingCalls() ),
								"useThreadDispatch",
								PyBool_FromLong( (int)PyStacklessIOEventQueue.GetUseThreadDispatch() ),
								"useOptionalThreadDispatch",
								PyBool_FromLong( (int)PyStacklessIOEventQueue.GetUseOptionalThreadDispatch() ),
								"wakeupEventActive",
								PyBool_FromLong( s_wakeupEventActive ),
								"usingIOCP",
								PyBool_FromLong( 0 ) );
	PyCATCH( NULL )
}

static PyObject* ApplySettings( PyObject* self, PyObject* dict )
{
	if( !PyDict_Check( dict ) )
	{
		PyErr_SetString( PyExc_TypeError, "expected dict" );
		return 0;
	}
	PyTRY
		PyObject* v;
	v = PyDict_GetItemString( dict, "usePendingCalls" );
	if( v )
		PyStacklessIOEventQueue.SetUsePendingCalls( !!PyObject_IsTrue( v ) );
	v = PyDict_GetItemString( dict, "useThreadDispatch" );
	if( v )
		PyStacklessIOEventQueue.SetUseThreadDispatch( !!PyObject_IsTrue( v ) );
	v = PyDict_GetItemString( dict, "useOptionalThreadDispatch" );
	if( v )
		PyStacklessIOEventQueue.SetUseOptionalThreadDispatch( !!PyObject_IsTrue( v ) );
	v = PyDict_GetItemString( dict, "wakeupEventActive" );
	if( v )
		s_wakeupEventActive = !!PyObject_IsTrue( v );
	Py_RETURN_NONE;
	PyCATCH( NULL )
}

static PyMethodDef stacklessio_extentions[] = {
	{ "wait", (PyCFunction)wait, METH_VARARGS, 0 },
	{ "break_wait", (PyCFunction)break_wait, METH_NOARGS, 0 },
	{ "dispatch", (PyCFunction)dispatch, METH_NOARGS, 0 },
	{ "GetStats", (PyCFunction)GetStats, METH_NOARGS, 0 },
	{ "GetStatus", (PyCFunction)GetStatus, METH_NOARGS, 0 },
	{ "ClearStats", (PyCFunction)ClearStats, METH_NOARGS, 0 },
#ifdef _WIN32
	{ "GetWakeupEventHandle", (PyCFunction)GetWakeupEventHandle, METH_NOARGS, 0 },
#endif
	{ "GetSettings", (PyCFunction)GetSettings, METH_NOARGS, 0 },
	{ "ApplySettings", (PyCFunction)ApplySettings, METH_O, 0 },
	{ NULL, NULL } //sentinel
};

#ifdef USE_ASYNC_FILE
extern PyTypeObject PyAsyncFile_Type;
#endif
PyObject* PyInit_stacklessio( void )
{
#ifdef USE_ASYNC_FILE
	if( PyType_Ready( &PyAsyncFile_Type ) < 0 )
		return;
#endif

	/* Create the module and add the functions */
	static struct PyModuleDef moduleDef
	{
		PyModuleDef_HEAD_INIT,
			"stacklessio",
			"",
			-1,
			stacklessio_extentions
	};
	auto module = PyModule_Create( &moduleDef );
	if( module == nullptr )
	{
		return nullptr;
	}

#ifdef USE_ASYNC_FILE
	if( PyModule_AddObject( module, "asyncfile", (PyObject*)&PyAsyncFile_Type ) )
		return;
	Py_INCREF( &PyAsyncFile_Type );
#endif

	return module;
}

SCallbackEntry* g_packetCallbackChainPostDecompress = NULL;

#ifdef NO_CARBONIO
// These methods are required by BlueNet so that it compiles, but they aren't used on the client except for
// CioAddPacketCallbackPostDecompress / CioRemovePacketCallbackPostDecompress.
bool CioSendPacket( const long long fd, const char* data, const unsigned int len, const char* OOBData, const unsigned int OOBLen )
{
	return false;
}
unsigned int CioGetMaxPacketsize( const unsigned int len, const unsigned int OOBLen )
{
	return 0;
}
unsigned int CioFormatPacket( char* buf, const char* data, const unsigned int len, const char* OOBData, const unsigned int OOBLen )
{
	return 0;
}
bool CioSendFormattedPacket( const long long fd, const char* data, const unsigned int len )
{
	return false;
}

void CioAddPacketCallbackPostDecompress( CioDataCallback packetCallback )
{
	SCallbackEntry* entry = new( std::nothrow ) SCallbackEntry;
	entry->callback = packetCallback;
	entry->next = g_packetCallbackChainPostDecompress;
	g_packetCallbackChainPostDecompress = entry;
}

void CioRemovePacketCallbackPostDecompress( CioDataCallback packetCallback )
{
	SCallbackEntry* prev = 0;
	for( SCallbackEntry* entry = g_packetCallbackChainPostDecompress; entry; entry = entry->next )
	{
		if( entry->callback == packetCallback )
		{
			if( prev )
			{
				prev->next = entry->next;
			}
			else
			{
				g_packetCallbackChainPostDecompress = entry->next;
			}

			delete entry;
			break;
		}

		prev = entry;
	}
}

void CioSetErrorLogCallback( void ( *callback )( const char* msg ) )
{
}
void CioSetStatusLogCallback( void ( *callback )( const char* msg ) )
{
}

PyObject* CioSetWakeupMethod( int method )
{
	PyErr_SetString( PyExc_NotImplementedError, "Missing implementation" );
	return NULL;
}
PyObject* CioGetWakeupMethod()
{
	PyErr_SetString( PyExc_NotImplementedError, "Missing implementation" );
	return NULL;
}
#endif
