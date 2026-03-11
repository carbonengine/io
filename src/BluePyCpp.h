// Copyright © 2026 CCP ehf.
/* This file defines some handy utility classes and functions
 * for the use of the Python API from C++
 */

#ifndef _PYCPP_H_
#define _PYCPP_H_

#include <Python.h>

namespace Ccp
{

/**************************************************
 * CPP Python utilities.
 * Useful C++ classes to probram the Python API
 */

// A RAII class to ensure that we have the Python GIL
class PyGilEnsure
{
public:
    PyGilEnsure() : mReleased(false), mState(PyGILState_Ensure())
    {
    }

    ~PyGilEnsure() noexcept
    {
        Release();
    }

	PyGilEnsure(const PyGilEnsure &o) = delete;
	void operator=(const PyGilEnsure &o) = delete;

    void Release() noexcept
    {
        if (!mReleased) {
            mReleased = true;
            PyGILState_Release(mState);
        }
    }

private:
    PyGILState_STATE mState;
    bool mReleased;
};

}; //namespace Ccp

#endif // _PYCPP_H_
