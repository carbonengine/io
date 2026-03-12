// Copyright © 2026 CCP ehf.
/* A C api for stacklessio */
#ifndef _STACKLESSIO_API_H_
#define _STACKLESSIO_API_H_

#ifdef __cplusplus
extern "C"
{
#endif

	int PyStacklessIoDispatchEvents( const char* from );

#ifdef _WIN32
	HANDLE PyStacklessIoGetWakeupEventHandle();
#endif
	typedef struct PyStacklessIoStatus_t
	{
		size_t struct_size;
		int nNonRunnable;
		int nRunnable;
	} PyStacklessIoStatus_t;
	void PyStacklessIoGetStatus( PyStacklessIoStatus_t* status );

#ifdef __cplusplus
};
#endif

#endif
