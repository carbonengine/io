/* This file defines some handy utility classes and functions
 * for the use of the Python API from C++
 */

#ifndef _PYCPP_H_
#define _PYCPP_H_

#include "BluePlatform.h"

#include <Python.h>
#ifdef STACKLESS
#include <stackless_api.h>
#endif


#include <stddef.h> //size_t et al
#include <exception>
#include <typeinfo>
#include <string>
#include <memory>   //allocator


// Exporting of classes.  Decleration of exported templates
#if defined(Py_ENABLE_SHARED) || defined(__CYGWIN__)
#if defined(HAVE_DECLSPEC_DLL)
#ifdef Py_BUILD_CORE
// This class will be expoerted from the python dll
#define PyAPI_CLASS(_C) class __declspec(dllexport) _C
// This template is defined here
#define PyAPI_TEMPLATEDEF
#else
// This class will be imported from the python dll
#define PyAPI_CLASS(_C) class __declspec(dllimport) _C
// The template instance is defined elsewhere
#define PyAPI_TEMPLATEDEF extern
#endif
#endif
#endif

// Default settings for other platforms
#ifndef PyAPI_CLASS
#define PyAPI_CLASS(_C) class _C
#define PyAPI_TEMPLATEDEF
#endif


#ifdef _DEBUGGING_THREAD_STUFF_
//use this to see if we are manipulating python stuff on worker threads
#undef Py_DECREF
template<class T>
inline void Py_DECREF(T *op) {
    ccp::PyGilAssert();
    if (_Py_DEC_REFTOTAL  _Py_REF_DEBUG_COMMA
        --(op)->ob_refcnt != 0)
        _Py_CHECK_REFCNT(op)
    else
        _Py_Dealloc((PyObject *)(op));
}
#endif

namespace Ccp
{

/**************************************************
 * CPP Python utilities.
 * Useful C++ classes to probram the Python API
 */

//An inline to see if we have the thread lock
inline bool PyGilHave() {
    return PyGILState_Check() == 1;
}

inline void PyGilAssert() {
    _ASSERT(PyGilHave());
}

// Base classes for python memory allocation.  Use this to use the PyObject_Malloc
// memory allocation API for c++ structures

// Inherit from this class to have memory alloced using the python memory allocator.
class PyMalloc
{
public:
    void *operator new(size_t size) {
        PyGilAssert();
        void *mem = PyObject_Malloc(size);
        if (mem == 0)
            throw std::bad_alloc();
        return mem;
    }
    void operator delete(void *ptr) throw() {
        //operator delete is never called for null pointers
        PyGilAssert();
        PyObject_Free(ptr);
    }
    
    //nothrow versions
    void *operator new(size_t size, const std::nothrow_t &) throw(){
        PyGilAssert();
        return PyObject_Malloc(size);
    }
    // a corresponding placement delete is _required_ by the standard.
    void operator delete(void *ptr, const std::nothrow_t &) throw() {
        PyGilAssert();
        PyObject_Free(ptr);
    }

// array versions of the above
    void *operator new[] (size_t size) {
        PyGilAssert();
        void *mem = PyObject_Malloc(size);
        if (mem == 0)
            throw std::bad_alloc();
        return mem;
    }
    void operator delete[] (void *ptr) throw() {
        PyGilAssert();
        PyObject_Free(ptr);
    }
    void *operator new[] (size_t size, const std::nothrow_t &) throw(){
        PyGilAssert();
        return PyObject_Malloc(size);
    }
    void operator delete[] (void *ptr, const std::nothrow_t &) throw() {
        PyGilAssert();
        PyObject_Free(ptr);
    }
};

// An std::allocator class that uses the PyObject_Malloc api
// to get memory.
template <class T>
class PyAllocator : public std::allocator<T>
{
	using _p = std::allocator_traits<std::allocator<T> >;
public:
    void deallocate(typename _p::pointer _Ptr, typename _p::size_type){
        PyGilAssert();
        PyObject_Free(_Ptr);
    }

    typename _p::pointer allocate(typename _p::size_type _Count) {
        PyGilAssert();
        // integer overflow tests
        if (_Count <= 0)
            _Count = 0;
        else if (((size_t)(-1) / _Count) < sizeof(T))
            throw std::bad_alloc();
        void *mem = PyObject_Malloc(_Count * sizeof(T));
        if (mem == 0)
            throw std::bad_alloc();
        return static_cast<typename _p::pointer>(mem);
    }
    
    typename _p::pointer allocate(typename _p::size_type _Count, const void *) {
        return allocate(_Count);
    }
};    


// A simple smartpointer to own references.
// When initialized, it steals the given reference, useful for such
// Python apis that return a new reference.  Initialize with incref=true
// to take its own reference.
template <class T>
class PyGenericObjectPtr
{
public:
    PyGenericObjectPtr() throw() : p(0) {}
    
    // This is explicit so that initial reference semantics are
    // correct.  Most commonly one doesn't incref but takes over
    // the existing new reference.
    // Contrast this with e.g. intrusive_ptr or shared_ptr where
    // a new "raw" objects is considered to have a refcount of 0
    explicit PyGenericObjectPtr(T *_p, bool incref = false) : p(_p) {
        if (incref)
            Py_XINCREF(p);
    }
    
    PyGenericObjectPtr(PyGenericObjectPtr const &o) throw() : p(o.p) {
        Py_XINCREF(p);
    }
    
    template <class C>
    PyGenericObjectPtr(PyGenericObjectPtr<C> const &o) throw() : 
            p(reinterpret_cast<T*>(o.p))
    {
        Py_XINCREF(p);
    }
    
    ~PyGenericObjectPtr() throw() {
        Py_XDECREF(p);
    }

    PyGenericObjectPtr &operator = (PyGenericObjectPtr const &o) throw() {
        PyGenericObjectPtr(o).swap(*this);
        return *this;
    }

    template <class C>
    PyGenericObjectPtr &operator = (PyGenericObjectPtr<C> const &o) throw() {
        *this = PyGenericObjectPtr(o);
        return *this;
    }
    

    void swap(PyGenericObjectPtr &o) throw() {
        T *tmp = p;
        p = o.p;
        o.p = tmp;
    }

    T *NewRef() const throw() {
        Py_XINCREF(p);
        return p;
    }

    T *Detach() throw() {
        T *tmp = p;
        p = 0;
        return tmp;
    }

    void Clear() throw() {
        PyGenericObjectPtr().swap(*this);
    }

    operator T* () const throw() { return p;}

    T * operator -> () const {
        _ASSERT(p);
        return p;
    }

    T ** operator & () throw() {
        _ASSERT(!p);
        return &p;
    }

    T * get() const throw() {return p;}

    T *p;
};

// A common typedef.
typedef PyGenericObjectPtr<PyObject> PyObjectPtr;


// Helpers for exception handling.


// A virtual base class, for those exceptions that can raise to python
class PyRaisable
{
public:
    virtual PyObject *PyRaise() const throw () = 0;
};


// Raise this, when a python exception is set on the thread.
class PyError : public std::exception, public PyRaisable
{
public:
	// We "Fetch" the exception, because that allows C++ to catch it
	// and ignore it.  If we left it in the python state, it would
	// stick around.
    PyError() throw() {
        _ASSERT(PyErr_Occurred());
        PyErr_Fetch(&mExc, &mVal, &mTb);
        log();
    }
    ~PyError() throw() {}

    const char * what() const throw() {
        try {
            if (What.size() == 0) {
                PyObject *e, *v, *tb;
                PyErr_Fetch(&e, &v, &tb);
                PyObjectPtr m(PyImport_ImportModule("traceback"));
                if (m) {
                    PyObjectPtr lst(PyObject_CallMethod(m, (char*)"format_exception_only", (char*)"OO", mExc.get(), mVal.get()));
                    if (lst && PyList_Size(lst)>0) {
                        try {
                            What = PyUnicode_AsUTF8(PyList_GetItem(lst, 0));
                        } catch (...) {}
                    }
                }
                PyErr_Restore(e, v, tb);
            }
            return What.c_str();
        } catch (...) {
            return "";
        }
    }

    PyObject *PyRaise() const throw() {
        // Copy the references for handoff, since we can't modify "this" here.
        PyObjectPtr exc(mExc);
        PyObjectPtr val(mVal);
        PyObjectPtr tb(mTb);
        PyErr_Restore(exc.Detach(), val.Detach(), tb.Detach());
        _ASSERT(PyErr_Occurred());
        return 0;
    }

    bool Matches(PyObject *exc) const throw() {
        return PyErr_GivenExceptionMatches(mExc, exc) != 0;
    }

private:
    void log() const throw () {
#ifndef NDEBUG
        try {
            OutputDebugString((std::string("CCPUtils::PyError: ")+ what()).c_str());
        } catch (...) {}
#endif
    }

private:
    mutable std::string What;
    PyObjectPtr mExc, mVal, mTb;
};


//raiser functions, for convenience
template<class T>
inline T PyCheck(T cmp, T val) {
    if (cmp == val)
        throw PyError(); //there was a python exception
    return val;
}
//special case for PyObject
template<class T> //deal with different PyObject types
inline T *PyCheck(T *obj) {
    if (!obj)
        throw PyError();
    return obj;
}

//specialization for int, where -1 is commonly the error mode
inline int PyCheck(int val) {
    if (val == -1)
        throw PyError();
    return val;
}


//Translate c++ exceptions to python exceptions
inline PyObject *PyErrFromException() throw() {
    PyErr_SetString(PyExc_RuntimeError, "unknown c++ exception");
    return 0;
}

inline PyObject *PyErrFromException(const std::exception &e) throw() {
    if (dynamic_cast<const std::bad_alloc *>(&e)) {
        PyErr_NoMemory();
        return NULL;
    }
    if (dynamic_cast<const PyRaisable *>(&e))
        return dynamic_cast<const PyRaisable *>(&e)->PyRaise();
    if (dynamic_cast<const Ccp::SystemError *>(&e)) {
        const Ccp::SystemError &err = dynamic_cast<const Ccp::SystemError &>(e);
#ifdef _WIN32
        PyErr_SetFromWindowsErrWithFilename(err.code(), err.what());
#else
        errno = err.code();
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, err.what());
#endif
        return NULL;
    }
    try {
        std::string tmp = std::string(typeid(e).name()) + ":" + e.what();
        PyErr_SetString(PyExc_RuntimeError, tmp.c_str());
        return NULL;
    } catch (...) {
        return PyErrFromException();
    }
}


// A RAII class to ensure that we have the Python GIL
class PyGilEnsure
{
public:
    PyGilEnsure(bool block=true) : mReleased(false), mState(PyGILState_Ensure())
    {
    }

    ~PyGilEnsure() throw()
    {
        Release();
    }

    bool Locked() const throw()
    {
        return mState == PyGILState_LOCKED;
    }

    void Release()
    {
        if (!mReleased) {
            mReleased = true;
            PyGILState_Release(mState);
        }
    }


private:
    PyGilEnsure(const PyGilEnsure &o);
    void operator=(const PyGilEnsure &o);
    PyGILState_STATE mState;
    bool mReleased;
};

// A RAII helper class to release the python GIL
// normally, safe == false, and this is used in a python context
// to demark a section where we release the GIL.
// If safe==true, it can be used from any context, whether we have
// the GIL or not.  Useful for functions that can be called either
// from Python or other threads.
class PyAllowThreads
{
public:
    PyAllowThreads(bool safe = false) : mState(nullptr), mReleased(false) {
        if (!safe) {
            //will check that we are in a python context
            PyGilAssert();
            mState = PyEval_SaveThread();
        } else {
            //only do it if we have the state.  Can be done in
            if (PyGilHave() && PyEval_ThreadsInitialized())
                mState = PyEval_SaveThread();
        }
    }
    ~PyAllowThreads() throw() {
        Release();
    }
    void Release() {
        if (!mReleased) {
            mReleased = true;
            if (mState)
                PyEval_RestoreThread(mState);
        }
    }

private:
    //no assignment, copy
    PyAllowThreads(const PyAllowThreads &o);
    PyAllowThreads &operator=(const PyAllowThreads &o);

    PyThreadState *mState;
    bool mReleased;
};


}; //namespace Ccp

#endif // _PYCPP_H_
