#include <gtest/gtest.h>

#include "seekdb.h"
#include "seekdb_internal.h"

#include <cstdio>

TEST(ConnectionOptions, RejectsNullArguments)
{
    SeekdbHandleImpl handle = {};
    SeekdbConnectionOptions options = {};

    EXPECT_EQ(seekdb_connection_options(nullptr, &options), SEEKDB_INVALID_ARGUMENT);
    EXPECT_EQ(seekdb_connection_options((SeekdbHandle)&handle, nullptr), SEEKDB_INVALID_ARGUMENT);
}

TEST(ConnectionOptions, ReturnsTcpPortAndUser)
{
    SeekdbHandleImpl handle = {};
    handle.port = 3306;
    SeekdbConnectionOptions options = {};

    ASSERT_EQ(seekdb_connection_options((SeekdbHandle)&handle, &options), SEEKDB_SUCCESS);
    EXPECT_STREQ(options.transport, SEEKDB_CONNECTION_TRANSPORT_TCP);
    EXPECT_EQ(options.port, 3306U);
    EXPECT_EQ(options.endpoint, nullptr);
    ASSERT_NE(options.user, nullptr);
    EXPECT_STREQ(options.user, "root");
}

#ifdef _WIN32
TEST(ConnectionOptions, ReturnsNamedPipeEndpoint)
{
    SeekdbHandleImpl handle = {};
    std::snprintf(handle.pipe_path, sizeof(handle.pipe_path), "\\\\.\\pipe\\seekdb-test");
    SeekdbConnectionOptions options = {};

    ASSERT_EQ(seekdb_connection_options((SeekdbHandle)&handle, &options), SEEKDB_SUCCESS);
    EXPECT_STREQ(options.transport, SEEKDB_CONNECTION_TRANSPORT_NAMED_PIPE);
    EXPECT_EQ(options.port, 0U);
    ASSERT_NE(options.endpoint, nullptr);
    EXPECT_STREQ(options.endpoint, "\\\\.\\pipe\\seekdb-test");
    EXPECT_STREQ(options.user, "root");
}
#else
TEST(ConnectionOptions, ReturnsUnixSocketEndpoint)
{
    SeekdbHandleImpl handle = {};
    char sock_path[] = "/tmp/seekdb/run/sql.sock";
    handle.sock_path = sock_path;
    SeekdbConnectionOptions options = {};

    ASSERT_EQ(seekdb_connection_options((SeekdbHandle)&handle, &options), SEEKDB_SUCCESS);
    EXPECT_STREQ(options.transport, SEEKDB_CONNECTION_TRANSPORT_UNIX_SOCKET);
    EXPECT_EQ(options.port, 0U);
    ASSERT_NE(options.endpoint, nullptr);
    EXPECT_STREQ(options.endpoint, "/tmp/seekdb/run/sql.sock");
    EXPECT_STREQ(options.user, "root");
}
#endif
