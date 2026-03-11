// Copyright © 2026 CCP ehf.

#include "carbonio.h"

void cb_alloc( uv_handle_t* handle, size_t size, uv_buf_t* buf )
{
	buf->base = new char[size];
	buf->len = size;
}

void cb_read( uv_stream_t* client, ssize_t nread, const uv_buf_t* buf )
{
	ON_BLOCK_EXIT( [&] {
		uv_read_stop( client );
		delete buf->base;
		uv_close( reinterpret_cast<uv_handle_t*>( client ), nullptr );
	} );

	ASSERT_EQ( 18, nread );
	ASSERT_STREQ( "\x10\x00\x00\x0E\x00\x00\x00\x05WorldHello", buf->base );
}

TEST( Socket, SendPacket )
{
	uv_os_sock_t sockets[2];
	uv_tcp_t handles[2];

	auto loop = uv_default_loop();

	ASSERT_EQ( 0, uv_socketpair( SOCK_STREAM, 0, sockets, 0, 0 ) );

	for ( size_t i = 0; i < 2; ++i )
	{
		ASSERT_EQ( 0, uv_tcp_init( loop, &handles[i] ) );
		ASSERT_EQ( 0, uv_tcp_open( &handles[i], sockets[i] ) );
	}

	AddToLookupTable( sockets[0], reinterpret_cast<uv_handle_t*>( &handles[0] ) );
	ASSERT_EQ( 1, SendPacket( sockets[0], "Hello", 5, "World", 5 ) );
	uv_close( reinterpret_cast<uv_handle_t*>( &handles[0] ), nullptr );

	uv_read_start( reinterpret_cast<uv_stream_t*>( &handles[1] ), cb_alloc, cb_read );

	uv_run( loop, UV_RUN_DEFAULT );
}

int main(int argc, char** argv)
{
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
