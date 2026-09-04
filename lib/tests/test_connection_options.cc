#include <gtest/gtest.h>

#include "port.h"
#include "seekdb.h"
#include "seekdb_internal.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

TEST(ConnectionOptions, RejectsNullArguments)
{
    SeekdbHandleImpl handle = {};
    SeekdbConnectionOptions options = {};

    EXPECT_EQ(seekdb_connection_options(nullptr, &options), SEEKDB_INVALID_ARGUMENT);
    EXPECT_EQ(seekdb_connection_options((SeekdbHandle)&handle, nullptr), SEEKDB_INVALID_ARGUMENT);
}

#ifdef _WIN32
TEST(ConnectionOptions, ReturnsTcpHostPortAndUser)
{
    SeekdbHandleImpl handle = {};
    std::snprintf(handle.host, sizeof(handle.host), "127.0.0.1");
    handle.port = 3306;
    std::snprintf(handle.server_uuid, sizeof(handle.server_uuid), "server-uuid");
    SeekdbConnectionOptions options = {};

    ASSERT_EQ(seekdb_connection_options((SeekdbHandle)&handle, &options), SEEKDB_SUCCESS);
    EXPECT_STREQ(options.transport, SEEKDB_CONNECTION_TRANSPORT_TCP);
    EXPECT_EQ(options.port, 3306U);
    ASSERT_NE(options.host, nullptr);
    EXPECT_STREQ(options.host, "127.0.0.1");
    EXPECT_EQ(options.unix_socket, nullptr);
    EXPECT_EQ(options.named_pipe, nullptr);
    ASSERT_NE(options.user, nullptr);
    EXPECT_STREQ(options.user, "root");
}

TEST(ConnectionOptions, ReturnsNamedPipeAndUser)
{
    SeekdbHandleImpl handle = {};
    std::snprintf(handle.pipe_name, sizeof(handle.pipe_name), "seekdb-test");
    std::snprintf(handle.pipe_path, sizeof(handle.pipe_path), "\\\\.\\pipe\\seekdb-test");
    SeekdbConnectionOptions options = {};

    ASSERT_EQ(seekdb_connection_options((SeekdbHandle)&handle, &options), SEEKDB_SUCCESS);
    EXPECT_STREQ(options.transport, SEEKDB_CONNECTION_TRANSPORT_NAMED_PIPE);
    EXPECT_EQ(options.port, 0U);
    EXPECT_EQ(options.host, nullptr);
    EXPECT_EQ(options.unix_socket, nullptr);
    ASSERT_NE(options.named_pipe, nullptr);
    EXPECT_STREQ(options.named_pipe, "\\\\.\\pipe\\seekdb-test");
    ASSERT_NE(options.user, nullptr);
    EXPECT_STREQ(options.user, "root");
}
#else
TEST(ConnectionOptions, ReturnsUnixSocketAndUser)
{
    SeekdbHandleImpl handle = {};
    handle.sock_path = (char *)"/tmp/seekdb-test/run/sql.sock";
    SeekdbConnectionOptions options = {};

    ASSERT_EQ(seekdb_connection_options((SeekdbHandle)&handle, &options), SEEKDB_SUCCESS);
    EXPECT_STREQ(options.transport, SEEKDB_CONNECTION_TRANSPORT_UNIX_SOCKET);
    EXPECT_EQ(options.port, 0U);
    EXPECT_EQ(options.host, nullptr);
    ASSERT_NE(options.unix_socket, nullptr);
    EXPECT_STREQ(options.unix_socket, "/tmp/seekdb-test/run/sql.sock");
    EXPECT_EQ(options.named_pipe, nullptr);
    ASSERT_NE(options.user, nullptr);
    EXPECT_STREQ(options.user, "root");
}
#endif

TEST(ConnectionOptions, RejectsUndiscoveredServer)
{
    SeekdbHandleImpl handle = {};
    SeekdbConnectionOptions options = {};

    EXPECT_EQ(seekdb_connection_options((SeekdbHandle)&handle, &options), SEEKDB_INTERNAL_ERROR);
}

#ifdef _WIN32
TEST(ConnectionOptions, RejectsMissingVerifiedIdentity)
{
    SeekdbHandleImpl handle = {};
    std::snprintf(handle.host, sizeof(handle.host), "127.0.0.1");
    handle.port = 3306;
    SeekdbConnectionOptions options = {};

    EXPECT_EQ(seekdb_connection_options((SeekdbHandle)&handle, &options), SEEKDB_INTERNAL_ERROR);
}
#endif

TEST(LifecyclePrimitives, TryLockDoesNotBlock)
{
    namespace fs = std::filesystem;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path lock_path =
        fs::temp_directory_path() / ("seekdb-flock-" + std::to_string(nonce) + ".lock");

    Flock *owner = nullptr;
    Flock *contender = nullptr;
    ASSERT_EQ(flock_open(lock_path.string().c_str(), &owner), OK);
    ASSERT_EQ(flock_open(lock_path.string().c_str(), &contender), OK);
    ASSERT_EQ(flock_try_acquire(owner, FLOCK_EXCLUSIVE), 1);

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(flock_try_acquire(contender, FLOCK_SHARED), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));

    flock_release(owner);
    EXPECT_EQ(flock_try_acquire(contender, FLOCK_SHARED), 1);
    EXPECT_EQ(flock_close(contender), OK);
    EXPECT_EQ(flock_close(owner), OK);
    std::error_code ec;
    fs::remove(lock_path, ec);
}
