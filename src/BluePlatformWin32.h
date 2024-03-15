//
//  win32.h
//  ccpUtils
//
//  Created by Kristjan Valur Jonsson on 17/03/14.
//
//

#ifndef ccputils_win32_h
#define ccputils_win32_h

#if !defined _WIN32_WINNT || _WIN32_WINNT < 0x0400
#define _WIN32_WINNT 0x0400
#endif
#include <windows.h>

#include <crtdbg.h>

#include <exception>
#include <string>

namespace Ccp
{
    
    // A standard place to report errors that we cannot raise.
    // We can add more channels here as we like!
    
    inline void ReportUnraisable() throw()
    {
        try {
            _RPT0(_CRT_ERROR , "Unraisable C++ exception.");
            OutputDebugString("Unraisable C++ exception.\n");
        } catch(...) {
            ; // give up
        }
    }
    inline void ReportUnraisable(const std::exception &e)
    {
        try {
            _RPT1(_CRT_ERROR , "Unraisable C++ exception: %s", e.what());
            OutputDebugString("Unraisable C++ exception: ");
            OutputDebugString(e.what());
            OutputDebugString("\n");
        } catch(...) {
            ReportUnraisable();
        }
    }
    
    
    // A win32 exception
    class SystemError : public std::runtime_error
    {
    public:
        SystemError(DWORD _code, const char *_msg) : runtime_error(_msg), error_code(_code) {log();}
        SystemError(DWORD _code) : runtime_error(""), error_code(_code) {log();}
        SystemError(const char *_msg) : runtime_error(_msg), error_code(GetLastError()) {log();}
        SystemError() : runtime_error(""), error_code(GetLastError()) {log();}
        
        DWORD code() const { return GetCode(); }
        DWORD GetCode() const {return error_code;}
        
        const char *what() const
        {
            if (What.size())
                return What.c_str();
            
            //on-demand creation of error message.
            char *lpMsgBuf;
            DWORD nChars = FormatMessageA(
                                          FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                          FORMAT_MESSAGE_FROM_SYSTEM,
                                          NULL,
                                          error_code,
                                          MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                          (LPSTR) &lpMsgBuf,
                                          0, NULL );
            if (!nChars)
                lpMsgBuf = 0;
            try {
                char tmp[16];
                sprintf_s(tmp, 16, "%d", error_code);
                if (lpMsgBuf)
                    What = std::string(std::runtime_error::what()) + " failed with error " + tmp + ": " + lpMsgBuf;
                else
                    What = std::string(std::runtime_error::what()) + " failed with error " + tmp + ": (no message)";
            } catch (...) {
                if (lpMsgBuf)
                    LocalFree(lpMsgBuf);
                throw;
            }
            if(lpMsgBuf)
                LocalFree(lpMsgBuf);
            return What.c_str();
        }
        
    private:
        //This logs out the creation of the exception to the debug device.
        void log() const throw () {
            try {
                OutputDebugString((std::string("Ccp::SystemError: ") + what()).c_str());
            } catch (...) {
                ;
            }
        }
        
    private:
        DWORD error_code;
        mutable std::string What;
    };
    
    
    //A performance counter inline function, for convenience
    inline LONGLONG GetPerformanceCounter() {
        LARGE_INTEGER i;
        QueryPerformanceCounter(&i);
        return i.QuadPart;
    }
    
    
    class _Handle
    {
    public:
        typedef HANDLE _type;
        static const _type Invalid() {return INVALID_HANDLE_VALUE;}
        static const bool IsInvalid(_type h) {return h == INVALID_HANDLE_VALUE;}
        static void Close(_type h) {
            if (!CloseHandle(h))
                throw SystemError("CloseHandle");
        }
    };
    

    //a CritialSection object
    class Mutex
    {
    public:
        Mutex() {
            InitializeCriticalSection( &section );
        }

#if _WIN32_WINNT >= 0x0403
        Mutex(DWORD spincount) {
            if (!InitializeCriticalSectionAndSpinCount( &section, spincount ))
                throw SystemError("InitializeCriticalSectionAndSpinCount");
        }
#endif
        ~Mutex() throw() {
            DeleteCriticalSection( &section );
        }
        void Enter() {
            EnterCriticalSection( &section );
        }
#if _WIN32_WINNT >= 0x0400
        bool TryEnter() {
            return !!TryEnterCriticalSection( &section );
        }
#endif
        void Leave() throw(){
            LeaveCriticalSection( &section );
        }
        // A canonical Acquire/Release interface, for use
        // by template based RAII classes
        void Acquire() {Enter();}
        void Release() {Leave();}

    private:
        //no copying
        Mutex(const Mutex &);
        Mutex &operator=(const Mutex &);

    private:
        CRITICAL_SECTION section;
    };


    class Atomic32
    {
    public:
        typedef LONG value_t;

        Atomic32(value_t v = 0) : _value(v) {}
        value_t Increment() {
            return InterlockedIncrement(&_value);
        }
        value_t Decrement() {
            return InterlockedDecrement(&_value);
        }

        bool CompareExchange(value_t oldvalue, value_t newvalue)
        {
            value_t r = InterlockedCompareExchange(&_value, newvalue, oldvalue);
            return r == oldvalue;
        }
        value_t value() const { return _value; }

    private:
        value_t _value;
    };


    // performace time
    typedef LONGLONG performance_t;
    inline performance_t GetPerformanceTime() {
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        return li.QuadPart;
    }

    inline double PerformanceTimeToS(performance_t t)
    {
        static double factor = 0.0;
        if (factor == 0.0) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            factor = 1.0/(double)freq.QuadPart;
        }
        return (double)t * factor;
    }

    
}; //Namespace Ccp


#endif
