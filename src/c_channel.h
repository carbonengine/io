// Copyright © 2026 CCP ehf.
// A C exposure for PyChannelObject from carbon-scheduler
// Created by hrafng on 2024-05-31.

#ifndef IO_C_CHANNEL_H
#define IO_C_CHANNEL_H

#include <Python.h>


#ifdef __cplusplus
#include <Scheduler.h>
extern "C" PyChannelObject* PyChannel_New( PyTypeObject* );
extern "C" PyObject* PyChannel_Receive( PyChannelObject* );
extern "C" int PyChannel_GetBalance( PyChannelObject* );
extern "C" int PyChannel_Send( PyChannelObject*, PyObject* );
extern "C" int InitScheduler();
#else
typedef struct PyChannelObject PyChannelObject;
PyChannelObject* PyChannel_New( PyTypeObject* );
PyObject* PyChannel_Receive( PyChannelObject* );
int PyChannel_GetBalance( PyChannelObject* );
int PyChannel_Send( PyChannelObject*, PyObject* );
int InitScheduler();
#endif

#endif //IO_C_CHANNEL_H
