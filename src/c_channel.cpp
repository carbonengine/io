// Copyright © 2026 CCP ehf.
#include "c_channel.h"

SchedulerCAPI* s_scheduler;

extern "C" PyChannelObject* PyChannel_New( PyTypeObject* channelType )
{
	return s_scheduler->PyChannel_New( channelType );
}

extern "C" PyObject* PyChannel_Receive( PyChannelObject* channel )
{
	return s_scheduler->PyChannel_Receive( channel );
}

extern "C" int PyChannel_GetBalance( PyChannelObject* channel )
{
	return s_scheduler->PyChannel_GetBalance( channel );
}

extern "C" int PyChannel_Send( PyChannelObject* channel, PyObject* obj )
{
	return s_scheduler->PyChannel_Send( channel, obj );
}

extern "C" int InitScheduler()
{
	s_scheduler = SchedulerAPI();

	return s_scheduler == nullptr ? 0 : -1;
}
