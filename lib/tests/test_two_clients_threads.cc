// Two-client lifecycle scenarios in a single test process (each client is
// a thread):
//
//   TwoConcurrentClients     both clients call seekdb_open at the same
//                            time; the startup-race winner spawns, the
//                            loser reconnects or takes the fast path.
//   BArrivesAfterAStartup    B opens after A's seekdb_open returns, so
//                            B takes the "server already up" fast path.
//   ClientBSeesClientAWrite  A writes a row through its connection; B
//                            reads it back through its own connection.
//
// Env:
//   SEEKDB_BIN   path to the seekdb binary

#include <gtest/gtest.h>

#include "port.h"
#include "seekdb.h"
#include "seekdb_internal.h"
#include "test_utils.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

class TwoClientsOpen : public ::testing::Test {
  protected:
    std::string db_dir_;

    void SetUp() override
    {
        const char *bin = std::getenv("SEEKDB_BIN");
        ASSERT_NE(bin, nullptr) << "set SEEKDB_BIN to the seekdb binary";
        ASSERT_TRUE(fs::exists(bin));

        db_dir_ = make_per_test_db_dir(SEEKDB_TEST_DATA_ROOT);
        fs::create_directories(db_dir_);
    }

    void TearDown() override
    {
        // Each test cleans up the daemons it spawned in its own body
        // using the SeekdbHandleImpl::spawned_pid it captured. Nothing
        // to do here.
    }
};

// ---------------------------------------------------------------------------
// Two clients call seekdb_open concurrently. Whichever wins the
// seekdb.startup race spawns the server; the other takes the fast path
// (or loses the seekdb.pid race and reconnects). Both seekdb_open calls
// must return SEEKDB_SUCCESS.
// ---------------------------------------------------------------------------
TEST_F(TwoClientsOpen, TwoConcurrentClients)
{
    std::mutex m;
    std::condition_variable cv;
    bool a_opened = false, b_opened = false;
    bool close_signal = false;
    int a_open_rc = -1, b_open_rc = -1;
    std::vector<int64_t> spawned_pids;

    auto run_client = [&](int &open_rc, bool &opened_flag) {
        SeekdbHandle h = nullptr;
        open_rc = seekdb_open(db_dir_.c_str(), 0, &h);
        tlog("seekdb_open return %d\n", open_rc);

        if (open_rc == SEEKDB_SUCCESS && h != nullptr) {
            int64_t pid = ((SeekdbHandleImpl *)h)->spawned_pid;
            std::lock_guard<std::mutex> lk(m);
            spawned_pids.push_back(pid);
            tlog("saved spawned pid = %lld\n", (long long)pid);
        }

        {
            std::lock_guard<std::mutex> lk(m);
            opened_flag = true;
        }
        cv.notify_all();

        // Wait for the test thread to signal close. Holding the handle
        // here keeps SH on seekdb.clients held, so the daemon doesn't
        // shut down before both opens have been asserted.
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return close_signal; });
        }

        seekdb_close(h);
        tlog("seekdb_close called\n");
    };

    std::thread ta(run_client, std::ref(a_open_rc), std::ref(a_opened));
    std::thread tb(run_client, std::ref(b_open_rc), std::ref(b_opened));

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return a_opened && b_opened; });
    }

    tlog("a rc = %d\n", a_open_rc);
    tlog("b rc = %d\n", b_open_rc);

    ASSERT_EQ(a_open_rc, SEEKDB_SUCCESS) << "client A failed to seekdb_open";
    ASSERT_EQ(b_open_rc, SEEKDB_SUCCESS) << "client B failed to seekdb_open";

    // Both opens succeeded — let the threads proceed to seekdb_close.
    {
        std::lock_guard<std::mutex> lk(m);
        close_signal = true;
    }
    cv.notify_all();

    ta.join();
    tb.join();

    // Terminate every daemon either thread spawned, then loop until each
    // is reaped. spawned_pids contains one entry per successful
    // seekdb_open; entries equal to 0 took the fast path (no spawn) and
    // are treated as already reaped.
    for (int64_t pid : spawned_pids) {
        if (pid > 0 && !is_server_reaped(pid)) {
            terminate_process(pid, /*graceful=*/0);
#ifndef _WIN32
            // SIGKILL leaves a zombie until the parent reaps it; without this
            // is_server_reaped (kill(pid, 0)) sees the zombie as alive forever.
            waitpid((pid_t)pid, nullptr, 0);
#endif
        }
    }
    while (true) {
        bool all_reaped = true;
        for (int64_t pid : spawned_pids) {
            if (pid > 0 && !is_server_reaped(pid)) {
                all_reaped = false;
                break;
            }
        }
        if (all_reaped)
            break;
        std::this_thread::sleep_for(200ms);
    }
}

// ---------------------------------------------------------------------------
// Case 2: B arrives after A's seekdb_open has fully returned.
// ---------------------------------------------------------------------------
TEST_F(TwoClientsOpen, BArrivesAfterAStartup)
{
    std::mutex m;
    std::condition_variable cv;
    bool a_opened = false;
    bool b_opened = false;
    bool close_signal = false;

    SeekdbHandle h_a = nullptr, h_b = nullptr;
    SeekdbConnection c_a = nullptr, c_b = nullptr;
    int a_open_rc = -1, a_query_rc = -1;
    int b_open_rc = -1, b_query_rc = -1;
    std::vector<int64_t> spawned_pids;

    auto run_client = [&](SeekdbHandle &h, SeekdbConnection &c, int &open_rc, int &query_rc,
                          bool &opened_flag) {
        open_rc = seekdb_open(db_dir_.c_str(), 0, &h);
        if (open_rc == SEEKDB_SUCCESS) {
            int64_t pid = ((SeekdbHandleImpl *)h)->spawned_pid;
            {
                std::lock_guard<std::mutex> lk(m);
                spawned_pids.push_back(pid);
            }
            tlog("saved spawned pid = %lld\n", (long long)pid);
            if (seekdb_connect(h, nullptr, true, &c) == SEEKDB_SUCCESS) {
                SeekdbResult r = nullptr;
                query_rc = seekdb_query(c, "SELECT 1", 8, &r);
                if (r)
                    seekdb_result_free(r);
            }
        }
        {
            std::lock_guard<std::mutex> lk(m);
            opened_flag = true;
        }
        cv.notify_all();
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return close_signal; });
        }
        if (c)
            seekdb_disconnect(c);
        if (h)
            seekdb_close(h);
    };

    std::thread ta(run_client, std::ref(h_a), std::ref(c_a), std::ref(a_open_rc),
                   std::ref(a_query_rc), std::ref(a_opened));

    // Wait until A's seekdb_open has fully returned. Only then is it
    // guaranteed that B will take the "server already up" fast path and
    // skip the spawn logic entirely.
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return a_opened; });
    }
    ASSERT_EQ(a_open_rc, SEEKDB_SUCCESS);
    ASSERT_EQ(a_query_rc, SEEKDB_SUCCESS);

    const int64_t server_pid = ((SeekdbHandleImpl *)h_a)->spawned_pid;
    ASSERT_GT(server_pid, 0);
    ASSERT_FALSE(is_server_reaped(server_pid));

    std::thread tb(run_client, std::ref(h_b), std::ref(c_b), std::ref(b_open_rc),
                   std::ref(b_query_rc), std::ref(b_opened));

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return b_opened; });
    }
    ASSERT_EQ(b_open_rc, SEEKDB_SUCCESS);
    ASSERT_EQ(b_query_rc, SEEKDB_SUCCESS);

    {
        std::lock_guard<std::mutex> lk(m);
        close_signal = true;
    }
    cv.notify_all();
    ta.join();
    tb.join();

    // Terminate every daemon either thread spawned, then loop until each
    // is reaped. spawned_pids contains one entry per successful
    // seekdb_open; entries equal to 0 took the fast path (no spawn).
    for (int64_t pid : spawned_pids) {
        if (pid > 0 && !is_server_reaped(pid)) {
            terminate_process(pid, /*graceful=*/0);
#ifndef _WIN32
            // SIGKILL leaves a zombie until the parent reaps it; without this
            // is_server_reaped (kill(pid, 0)) sees the zombie as alive forever.
            waitpid((pid_t)pid, nullptr, 0);
#endif
        }
    }
    while (true) {
        bool all_reaped = true;
        for (int64_t pid : spawned_pids) {
            if (pid > 0 && !is_server_reaped(pid)) {
                all_reaped = false;
                break;
            }
        }
        if (all_reaped)
            break;
        std::this_thread::sleep_for(200ms);
    }
}

// Cross-client visibility: thread A writes a row, thread B reads it back
// through its own connection. Both threads live in the test process; each
// holds its own SeekdbHandle, and the test orchestrates the order of the
// writes and the read via a single mutex + cv.
TEST_F(TwoClientsOpen, ClientBSeesClientAWrite)
{
    std::mutex m;
    std::condition_variable cv;
    bool a_opened = false, b_opened = false;
    bool a_write_go = false, a_write_done = false;
    bool b_query_go = false, b_query_done = false;
    bool close_signal = false;

    int a_open_rc = -1, a_write_rc = -1;
    int b_open_rc = -1, b_query_rc = -1;
    int64_t b_seen_value = 0;
    std::vector<int64_t> spawned_pids;

    auto run_a = [&]() {
        SeekdbHandle h = nullptr;
        a_open_rc = seekdb_open(db_dir_.c_str(), 0, &h);
        if (a_open_rc == SEEKDB_SUCCESS && h != nullptr) {
            int64_t pid = ((SeekdbHandleImpl *)h)->spawned_pid;
            {
                std::lock_guard<std::mutex> lk(m);
                spawned_pids.push_back(pid);
            }
            tlog("saved spawned pid = %lld\n", (long long)pid);
        }
        {
            std::lock_guard<std::mutex> lk(m);
            a_opened = true;
        }
        cv.notify_all();
        if (a_open_rc != SEEKDB_SUCCESS) {
            if (h)
                seekdb_close(h);
            return;
        }

        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return a_write_go; });
        }

        SeekdbConnection c = nullptr;
        a_write_rc = seekdb_connect(h, nullptr, true, &c);
        if (a_write_rc == SEEKDB_SUCCESS) {
            SeekdbResult r = nullptr;
            a_write_rc = seekdb_query(c, "USE test", 8, &r);
            if (r) {
                seekdb_result_free(r);
                r = nullptr;
            }
            if (a_write_rc == SEEKDB_SUCCESS)
                a_write_rc = seekdb_query(c, "CREATE TABLE t1(v int)", 22, &r);
            if (r) {
                seekdb_result_free(r);
                r = nullptr;
            }
            if (a_write_rc == SEEKDB_SUCCESS)
                a_write_rc = seekdb_query(c, "INSERT INTO t1 VALUES (1)", 25, &r);
            if (r) {
                seekdb_result_free(r);
                r = nullptr;
            }
            seekdb_disconnect(c);
        }

        {
            std::lock_guard<std::mutex> lk(m);
            a_write_done = true;
        }
        cv.notify_all();

        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return close_signal; });
        }
        seekdb_close(h);
    };

    auto run_b = [&]() {
        SeekdbHandle h = nullptr;
        b_open_rc = seekdb_open(db_dir_.c_str(), 0, &h);
        if (b_open_rc == SEEKDB_SUCCESS && h != nullptr) {
            int64_t pid = ((SeekdbHandleImpl *)h)->spawned_pid;
            {
                std::lock_guard<std::mutex> lk(m);
                spawned_pids.push_back(pid);
            }
            tlog("saved spawned pid = %lld\n", (long long)pid);
        }
        {
            std::lock_guard<std::mutex> lk(m);
            b_opened = true;
        }
        cv.notify_all();
        if (b_open_rc != SEEKDB_SUCCESS) {
            if (h)
                seekdb_close(h);
            return;
        }

        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return b_query_go; });
        }

        SeekdbConnection c = nullptr;
        b_query_rc = seekdb_connect(h, nullptr, true, &c);
        if (b_query_rc == SEEKDB_SUCCESS) {
            SeekdbResult r = nullptr;
            b_query_rc = seekdb_query(c, "SELECT * FROM test.t1", 21, &r);
            if (b_query_rc == SEEKDB_SUCCESS) {
                if (seekdb_result_next(r) != SEEKDB_SUCCESS)
                    b_query_rc = SEEKDB_INTERNAL_ERROR;
                else if (seekdb_result_get_int64(r, 0, &b_seen_value) != SEEKDB_SUCCESS)
                    b_query_rc = SEEKDB_INTERNAL_ERROR;
            }
            if (r)
                seekdb_result_free(r);
            seekdb_disconnect(c);
        }

        {
            std::lock_guard<std::mutex> lk(m);
            b_query_done = true;
        }
        cv.notify_all();

        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return close_signal; });
        }
        seekdb_close(h);
    };

    std::thread ta(run_a);
    std::thread tb(run_b);

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return a_opened && b_opened; });
    }
    ASSERT_EQ(a_open_rc, SEEKDB_SUCCESS);
    ASSERT_EQ(b_open_rc, SEEKDB_SUCCESS);

    {
        std::lock_guard<std::mutex> lk(m);
        a_write_go = true;
    }
    cv.notify_all();
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return a_write_done; });
    }
    ASSERT_EQ(a_write_rc, SEEKDB_SUCCESS) << "A's USE/CREATE/INSERT sequence failed";

    {
        std::lock_guard<std::mutex> lk(m);
        b_query_go = true;
    }
    cv.notify_all();
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return b_query_done; });
    }
    ASSERT_EQ(b_query_rc, SEEKDB_SUCCESS) << "B's SELECT failed";
    EXPECT_EQ(b_seen_value, 1) << "B did not read the row A inserted";

    {
        std::lock_guard<std::mutex> lk(m);
        close_signal = true;
    }
    cv.notify_all();
    ta.join();
    tb.join();

    // Terminate every daemon either thread spawned, then loop until each
    // is reaped. spawned_pids contains one entry per successful
    // seekdb_open; entries equal to 0 took the fast path (no spawn).
    for (int64_t pid : spawned_pids) {
        if (pid > 0 && !is_server_reaped(pid)) {
            terminate_process(pid, /*graceful=*/0);
#ifndef _WIN32
            // SIGKILL leaves a zombie until the parent reaps it; without this
            // is_server_reaped (kill(pid, 0)) sees the zombie as alive forever.
            waitpid((pid_t)pid, nullptr, 0);
#endif
        }
    }
    while (true) {
        bool all_reaped = true;
        for (int64_t pid : spawned_pids) {
            if (pid > 0 && !is_server_reaped(pid)) {
                all_reaped = false;
                break;
            }
        }
        if (all_reaped)
            break;
        std::this_thread::sleep_for(200ms);
    }
}

} // namespace
