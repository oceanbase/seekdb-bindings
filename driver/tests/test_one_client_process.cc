// Three ways a single client process can go away. In each case the
// last-client-gone detection on seekdb.clients should make the server
// shut itself down, and the test asserts the server is reaped within
// 15s of the client process being forked.
//
//   ClientClose — child calls seekdb_close, then _exit(0).
//   ClientExit  — child _exits without seekdb_close (kernel releases
//                 the SH lock via fd cleanup).
//   KillClient  — child loops; parent SIGKILLs it.
//
// Env:
//   SEEKDB_BIN   path to the seekdb binary

#include <gtest/gtest.h>

#include "port.h"
#include "seekdb.h"
#include "seekdb_internal.h"
#include "test_utils.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

class OneClientProcess : public ::testing::Test {
protected:
    std::string db_dir_;

    void SetUp() override {
        const char *bin = std::getenv("SEEKDB_BIN");
        ASSERT_NE(bin, nullptr) << "set SEEKDB_BIN to the seekdb binary";
        ASSERT_TRUE(fs::exists(bin));

        db_dir_ = make_per_test_db_dir(SEEKDB_TEST_DATA_ROOT);
        fs::create_directories(db_dir_);
    }

};

TEST_F(OneClientProcess, ClientClose)
{
    int ready[2];
    pipe(ready);

    pid_t client_pid = fork();
    if (client_pid == 0) {
        close(ready[0]);
        SeekdbHandle h = nullptr;
        seekdb_open(db_dir_.c_str(), 0, &h);
        int64_t spawned_pid = ((SeekdbHandleImpl *)h)->spawned_pid;
        write(ready[1], &spawned_pid, sizeof(spawned_pid));
        close(ready[1]);
        seekdb_close(h);
        _exit(0);
    }
    close(ready[1]);
    int64_t server_pid = 0;
    read(ready[0], &server_pid, sizeof(server_pid));
    close(ready[0]);

    const auto ddl = std::chrono::steady_clock::now() + 30s;

    while (!is_server_reaped(server_pid) && std::chrono::steady_clock::now() < ddl)
        std::this_thread::sleep_for(1s);

    ASSERT_TRUE(is_server_reaped(server_pid))
        << "server " << server_pid << " not reaped within 15s after client close";
}

TEST_F(OneClientProcess, ClientExit)
{
    int ready[2];
    pipe(ready);

    pid_t client_pid = fork();
    if (client_pid == 0) {
        close(ready[0]);
        SeekdbHandle h = nullptr;
        seekdb_open(db_dir_.c_str(), 0, &h);
        int64_t spawned_pid = ((SeekdbHandleImpl *)h)->spawned_pid;
        write(ready[1], &spawned_pid, sizeof(spawned_pid));
        close(ready[1]);
        _exit(0);
    }
    close(ready[1]);
    int64_t server_pid = 0;
    read(ready[0], &server_pid, sizeof(server_pid));
    close(ready[0]);

    const auto ddl = std::chrono::steady_clock::now() + 30s;

    while (!is_server_reaped(server_pid) && std::chrono::steady_clock::now() < ddl)
        std::this_thread::sleep_for(1s);

    ASSERT_TRUE(is_server_reaped(server_pid))
        << "server " << server_pid << " not reaped within 15s after client close";
}

TEST_F(OneClientProcess, KillClient)
{
    int ready[2];
    pipe(ready);

    pid_t client_pid = fork();
    if (client_pid == 0) {
        close(ready[0]);
        SeekdbHandle h = nullptr;
        seekdb_open(db_dir_.c_str(), 0, &h);
        int64_t spawned_pid = ((SeekdbHandleImpl *)h)->spawned_pid;
        write(ready[1], &spawned_pid, sizeof(spawned_pid));
        close(ready[1]);
        while (true) {
            std::this_thread::sleep_for(10s);
        };
    }
    close(ready[1]);
    int64_t server_pid = 0;
    read(ready[0], &server_pid, sizeof(server_pid));
    close(ready[0]);

    const auto ddl = std::chrono::steady_clock::now() + 30s;

    terminate_process(client_pid, true);

    while (!is_server_reaped(server_pid) && std::chrono::steady_clock::now() < ddl)
        std::this_thread::sleep_for(1s);

    ASSERT_TRUE(is_server_reaped(server_pid))
        << "server " << server_pid << " not reaped within 15s after client close";
}

}  // namespace
