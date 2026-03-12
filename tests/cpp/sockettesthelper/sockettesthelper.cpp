// Copyright © 2026 CCP ehf.

#define PY_SSIZE_T_CLEAN // Recommended by C/C++ extension tutorial in Python docs
#include <Python.h>
#include <socketmodule.h>

int PacketEatingOobDataCallback( long long descriptor, const char* data, int len, const char* OOBdata, int OOBLen )
{
	if( OOBLen == 5 && !strncmp( OOBdata, "eatme", 5 ) )
	{
		return 1;
	}
	return 0;
}

static PyObject* sockethelper_addpacketeatingcallback( PyObject* self, PyObject* args )
{
	auto* cApi = reinterpret_cast<PySocketModule_APIObject*>( PySocketModule_ImportModuleAndAPI() );
	if( !cApi )
	{
		PyErr_SetString( PyExc_RuntimeError, "Failed to import _socket module C API" );
		return nullptr;
	}
	cApi->add_oob_data_callback( PacketEatingOobDataCallback );
	Py_RETURN_TRUE;
}

static PyObject* sockethelper_removepacketeatingcallback( PyObject* self, PyObject* args )
{
	auto* cApi = reinterpret_cast<PySocketModule_APIObject*>( PySocketModule_ImportModuleAndAPI() );
	if( !cApi )
	{
		PyErr_SetString( PyExc_RuntimeError, "Failed to import _socket module C API" );
		return nullptr;
	}
	cApi->remove_oob_data_callback( PacketEatingOobDataCallback );
	Py_RETURN_TRUE;
}

static PyMethodDef SocketTestHelperMethods[] = {
	{ "addpacketeatingcallback", sockethelper_addpacketeatingcallback, METH_VARARGS, "Install a callback that eats oob data packets of length 5 that contain the string 'eatme'." },
	{ "removepacketeatingcallback", sockethelper_removepacketeatingcallback, METH_VARARGS, "Clean up the packet eating callback installed by 'addpacketeatingcallback'." },
	{ nullptr, nullptr, 0, nullptr }
};

static struct PyModuleDef sockettesthelpermodule = {
	PyModuleDef_HEAD_INIT,
	"sockettesthelper",
	"Helper module that interacts with exposed C API functions in order to "
	"allow Python tests to cover cases that have to do with those",
	-1,
	SocketTestHelperMethods
};

PyMODINIT_FUNC PyInit_sockettesthelper( void )
{
	return PyModule_Create( &sockettesthelpermodule );
}
