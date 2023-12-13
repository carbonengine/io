#include <uv.h>

void foo()
{
	auto loop = uv_default_loop();
	uv_run( loop, UV_RUN_DEFAULT );
	uv_loop_close( loop );
}
