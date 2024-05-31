#include "c_channel.h"

extern "C" PyChannelObject* PyChannel_New( PyTypeObject* channelType )
{
	return SchedulerAPI()->PyChannel_New( channelType );
}

extern "C" PyObject* PyChannel_Receive( PyChannelObject* channel )
{
	return SchedulerAPI()->PyChannel_Receive( channel );
}

extern "C" int PyChannel_GetBalance( PyChannelObject* channel )
{
	return SchedulerAPI()->PyChannel_GetBalance( channel );
}

extern "C" int PyChannel_Send( PyChannelObject* channel, PyObject* obj )
{
	return SchedulerAPI()->PyChannel_Send( channel, obj );
}