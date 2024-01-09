//
//  posix.h
//
//  Posix related system wrappers
//
//  Created by Kristjan Valur Jonsson on 17/03/14.
//
//


#ifndef ccputils_posix_h
#define ccputils_posix_h

#include <stdio.h>
#include <unistd.h>
#include <sys/errno.h>
#include <sys/time.h>

#include <pthread.h>

#include <string>
#include <stdexcept>

#ifdef __APPLE__
#include <atomic>
#include <mach/mach_time.h> // for high resolution timer
#endif

// Various defines
#define _ASSERT assert

inline void OutputDebugString(const char *str) {
    fprintf(stderr, "%s\n", str);
}


namespace Ccp
{
    
    // A standard place to report errors that we cannot raise.
    // We can add more channels here as we like!

    inline void ReportUnraisable() throw()
    {
        try {
            fprintf(stderr, "Unraisable C++ exception.\n");
        } catch(...) {
            ; // give up
        }
    }
    inline void ReportUnraisable(const std::exception &e)
    {
        try {
            fprintf(stderr, "Unraisable C++ exception: %s\n", e.what());
        } catch(...) {
            ReportUnraisable();
        }
    }


    // A win32 exception
    class SystemError : public std::runtime_error
    {
    public:
        SystemError(int errnum, const std::string &msg) : std::runtime_error(msg), errorcode(errnum) { set_message();}
        SystemError(int errnum, const char *msg=NULL) : std::runtime_error(std::string(msg?msg:"")), errorcode(errnum) { set_message();}
        SystemError(const std::string &msg) : std::runtime_error(msg), errorcode(GetErrno()) { set_message();}
        SystemError(const char *msg=NULL) : std::runtime_error(std::string(msg?msg:"")), errorcode(GetErrno()) { set_message();}
        ~SystemError() throw() {}
        int code() const throw () {return errorcode;}
        int GetCode() const throw() {return code();}
        const char *what() const throw() {return message.c_str();}
        
    private:
        static int GetErrno() throw() { return errno; }
        void set_message() {
            char tmp[16];
            snprintf(tmp, sizeof(tmp)/sizeof(tmp[0]), "(%d)", errorcode);
            message = std::string("CCP:SystemError: ");
            const char *msg = runtime_error::what();
            if (strlen(msg))
                message = message + msg + ": ";
            message = message + strerror(errorcode) + tmp;
            log();
        }
        
        //This logs out the creation of the exception to the debug device.
        void log() const throw() {
            try {
    #ifndef NDEBUG
                fprintf(stderr, "%s\n", what());
    #else
                ;
    #endif
            } catch (...) {
                ;
            }
        }
    private:
        int errorcode;
        std::string message;
    };

    inline void UnraisableSystemError(int _code, const char *_msg)
    {
        try {
            throw SystemError(_code, _msg);
        } catch (const SystemError &e) {
            ReportUnraisable(e);
        }
    }


    //a CritialSection object
    class Mutex
    {
        friend class Condition;
    public:
        Mutex(bool recursive=false) {
            pthread_mutexattr_t attr;
            pthread_mutexattr_t *pattr = NULL;
            if (recursive) {
                int r = pthread_mutexattr_init(&attr);
                if (r)
                    throw SystemError(r, "pthread_mutexattr_init");
                r = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
                if (r)
                    throw SystemError(r, "pthread_mutexattr_settype");
                pattr = &attr;
            }
            int r = pthread_mutex_init(&mutex, pattr);
            if (r)
                throw SystemError(r, "pthread_mutex_init");
        }
        
        ~Mutex() throw() {
            int r = pthread_mutex_destroy(&mutex);
            if (r)
                UnraisableSystemError(r, "pthread_mutex_destroy");
        }

        void Acquire() {
            int r = pthread_mutex_lock(&mutex);
            if (r)
                throw SystemError(r, "pthread_mutex_lock");
        }
        bool TryAcquire() {
            int r = pthread_mutex_trylock(&mutex);
            if (r == EBUSY)
                return false;
            if (r)
                throw SystemError(r, "pthread_mutex_trylock");
            else
                return true;
        }
        void Release() throw(){
            int r = pthread_mutex_unlock(&mutex);
            if (r)
                UnraisableSystemError(r, "pthread_mutex_unlock");
        }
        
    protected:
        pthread_mutex_t *get() { return &mutex; }
        
    private:
        //no copying
        Mutex(const Mutex &);
        Mutex &operator=(const Mutex &);
        
    private:
        pthread_mutex_t mutex;
    };
    
    typedef Mutex CrititalSection; // for backwards compatibility
    
    
    class Condition
    {
    public:
        Condition() {
            int r = pthread_cond_init(&cond, NULL);
            if (r)
                throw SystemError(r, "pthread_cond_init");
            
        }
        ~Condition() {
            int r = pthread_cond_destroy(&cond);
            if (r)
                UnraisableSystemError(r, "pthread_cond_destroy");
        }
        
        void Signal() {
            int r = pthread_cond_signal(&cond);
            if (r)
                throw SystemError(r, "pthread_cond_signal");
        }
        
        void Broadcast() {
            int r = pthread_cond_broadcast(&cond);
            if (r)
                throw SystemError(r, "pthread_cond_broadcast");
        }
        void Wait(Mutex &m)
        {
            int r = pthread_cond_wait(&cond, m.get());
            if (r)
                throw SystemError(r, "pthread_cond_wait");
        }
        
        bool TimedWait(Mutex &m, const struct timespec &abstime)
        {
            int r = pthread_cond_timedwait(&cond, m.get(), &abstime);
            if (r == ETIMEDOUT)
                return false;
            if (r)
                throw SystemError(r, "pthread_cond_timedwait");
            return true;
        }
        
        struct deadline_t
        {
            struct timespec abstime;
        };
        
        bool TimedWait(Mutex &m, const deadline_t &deadline)
        {
            return TimedWait(m, deadline.abstime);
        }
        
        bool TimedWait(Mutex &m, double delay)
        {
            return TimedWait(m, ComputeDeadline(delay));
        }
        
        
        static deadline_t
        ComputeDeadline(double delay)
        {
            if (delay < 0.0)
                delay = 0.0;
            struct timeval now;
            struct timespec when;
            if (gettimeofday(&now, NULL))
                throw SystemError(errno, "gettimeofday");
            when.tv_sec = (time_t)delay;
            when.tv_nsec = (long)((delay - (double)(when.tv_sec)) * 1e9);
            when.tv_sec += now.tv_sec;
            when.tv_nsec += now.tv_usec*1000;
            if (when.tv_nsec >= 1000000000) {
                when.tv_nsec -= 1000000000;
                when.tv_sec += 1;
            }
            deadline_t result;
            result.abstime = when;
            return result;
        }
        
        
    private:
        //no copying
        Condition(const Condition &);
        Condition &operator=(const Condition &);
        
    private:
        pthread_cond_t cond;
    };

    
    
    class _Handle
    {
    public:
        typedef int _type;
        static const _type Invalid() {return -1;}
        static const bool IsInvalid(_type v) {return v < 0;}
        static void Close(_type h) {
            int r = close(h);
            if (r)
                throw SystemError("close");
        }
    };

    //Should get deleted, since we have std::atomic now instead
    template<typename T>
    class Atomic_type
    {
    public:
        typedef T value_t;
        
        Atomic_type<T>(T v = 0) : _value(v) {}

        T Increment(bool barrier=true)
        {
            if (barrier)
                return std::atomic_fetch_add_explicit(&_value, 1, std::memory_order_seq_cst);
            else
                return ++_value;
        }

        T Decrement(bool barrier=true)
        {
            if (barrier)
                return std::atomic_fetch_sub_explicit(&_value, 1, std::memory_order_seq_cst);
            else
                return --_value;
        }
        
        bool CompareExchange(T oldvalue, T newvalue, bool barrier=true)
        {
            if (barrier)
                return std::atomic_compare_exchange_weak_explicit(&_value, &oldvalue, newvalue, std::memory_order_seq_cst, std::memory_order_seq_cst);
            else
                return std::atomic_compare_exchange_weak(&_value, &oldvalue, newvalue);
        }

        T Add(T v, bool barrier=true)
        {
            if (barrier)
                return std::atomic_fetch_add_explicit(&_value, v, std::memory_order_seq_cst);
            else
                return _value += v;
        }
        
        T value() const { return _value; }

        Atomic_type<T> &operator=(const Atomic_type<T> &o)
        {
            _value = o._value.load();
            return *this;
        }
        
    private:
        std::atomic<value_t> _value;
    };
    
    typedef Atomic_type<int32_t> Atomic32;
    typedef Atomic_type<int64_t> Atomic64;

#ifdef __APPLE__
    // performace time
    typedef int64_t performance_t;
    inline performance_t GetPerformanceTime() {
        return mach_absolute_time();
    }
    
    inline double PerformanceTimeToS(performance_t t)
    {
        mach_timebase_info_data_t info;
        static double factor = 0.0;
        if (factor == 0.0) {
            mach_timebase_info(&info);
            factor = (double)info.numer / (double)info.denom; // to give nanoseconds
            factor /= 1e-9; // to give seconds
        }
        return (double)t * factor;
    }
#endif

}; // namespace


#endif
