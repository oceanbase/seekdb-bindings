// Regression test for issue #26: parameters changed by the user must survive
// a restart and not be clobbered by the defaults the driver seeds on first
// init.
//
// seekdb_open seeds `--parameter memory_limit=1G --parameter log_disk_size=2G`
// only when the data dir is fresh (store/sstable empty). On a restart it must
// NOT pass --parameter, otherwise the command line overrides whatever the user
// changed and seekdb persisted. This test:
//
//   1. opens a fresh db_dir (first init seeds memory_limit=1G),
//   2. changes memory_limit via ALTER SYSTEM SET and confirms it took effect,
//   3. fully shuts the server down,
//   4. re-opens (restart path) and asserts memory_limit is still the changed
//      value — not reset back to the seeded default.
//
// Env:
//   SEEKDB_BIN   path to the seekdb binary
//
// POSIX-only: the spawned server is a direct child of this test process (the
// background reaper is disabled), so we reap it with waitpid — kill(pid, 0)
// would treat the not-yet-reaped zombie as still alive.

#include <gtest/gtest.h>

#include "port.h"
#include "seekdb.h"
#include "seekdb_internal.h"
#include "test_utils.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// Read the `value` of a cluster parameter via SHOW PARAMETERS. Columns are
// located by header name so the test doesn't hard-code SHOW PARAMETERS' column
// order. Returns "" if the parameter/row/value can't be read.
std::string read_parameter(SeekdbConnection c, const std::string &name)
{
    const std::string sql = "SHOW PARAMETERS LIKE '" + name + "'";
    SeekdbResult r = nullptr;
    if (seekdb_query(c, sql.c_str(), (int64_t)sql.size(), &r) != SEEKDB_SUCCESS) {
        if (r)
            seekdb_result_free(r);
        return "";
    }

    int64_t ncol = 0;
    seekdb_result_column_count(r, &ncol);
    int64_t name_idx = -1, value_idx = -1;
    for (int64_t i = 0; i < ncol; ++i) {
        const char *cn = nullptr;
        if (seekdb_result_column_name(r, i, &cn) == SEEKDB_SUCCESS && cn) {
            if (std::strcmp(cn, "name") == 0)
                name_idx = i;
            if (std::strcmp(cn, "value") == 0)
                value_idx = i;
        }
    }

    std::string out;
    if (value_idx >= 0) {
        while (seekdb_result_next(r) == SEEKDB_SUCCESS) {
            // SHOW PARAMETERS LIKE may match more than the exact name; keep
            // only the row whose `name` column equals the requested parameter.
            if (name_idx >= 0) {
                const char *nd = nullptr;
                size_t nl = 0;
                int nn = 0;
                if (seekdb_result_get_str(r, name_idx, &nd, &nl, &nn) == SEEKDB_SUCCESS && nd &&
                    std::string(nd, nl) != name) {
                    continue;
                }
            }
            const char *vd = nullptr;
            size_t vl = 0;
            int vn = 0;
            if (seekdb_result_get_str(r, value_idx, &vd, &vl, &vn) == SEEKDB_SUCCESS && vd) {
                out.assign(vd, vl);
                break;
            }
        }
    }

    seekdb_result_free(r);
    return out;
}

// Drop the client handle (releases the SH clients lock), then hard-kill the
// spawned daemon and reap it, so the next seekdb_open is forced down the spawn
// (restart) path. seekdb does not stop on SIGTERM, so we SIGKILL (graceful=0);
// the memory_limit change is already persisted by the time we get here (we let
// it settle before shutting down), so a hard kill is safe for this test.
void shutdown_server(SeekdbHandle h, int64_t pid)
{
    seekdb_close(h);
    terminate_process(pid, /*graceful=*/0);

    bool reaped = false;
    const auto ddl = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < ddl) {
        int status = 0;
        pid_t r = waitpid((pid_t)pid, &status, WNOHANG);
        if (r == (pid_t)pid || (r == -1 && errno == ECHILD)) {
            reaped = true;
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(reaped) << "spawned server (pid " << pid << ") did not exit within 30s of SIGKILL";
}

class ParameterPersistence : public ::testing::Test {
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
};

TEST_F(ParameterPersistence, ChangedMemoryLimitSurvivesRestart)
{
    // --- first init: store/sstable is empty, so the driver seeds the default
    //     --parameter memory_limit=1G ---
    SeekdbHandle h1 = nullptr;
    ASSERT_EQ(seekdb_open(db_dir_.c_str(), NULL, &h1), SEEKDB_SUCCESS);
    ASSERT_NE(h1, nullptr);
    const int64_t pid1 = ((SeekdbHandleImpl *)h1)->spawned_pid;
    ASSERT_GT(pid1, 0) << "first open should have spawned a fresh server";

    // init must have populated the data dir — this is the very signal the
    // driver uses to tell "first init" from "restart".
    const std::string sstable_dir = db_dir_ + "/store/sstable";
    EXPECT_TRUE(dir_has_entries(sstable_dir.c_str()))
        << "store/sstable should be populated after first init";

    SeekdbConnection c1 = nullptr;
    ASSERT_EQ(seekdb_connect(h1, nullptr, true, &c1), SEEKDB_SUCCESS);

    const std::string seeded = read_parameter(c1, "memory_limit");
    ASSERT_FALSE(seeded.empty()) << "could not read memory_limit after first init";

    // Change it to a non-default value. '2G' differs from the seeded 1G, so a
    // restart that wrongly re-seeds 1G would be detectable.
    const char *alter = "ALTER SYSTEM SET memory_limit = '2G'";
    SeekdbResult r = nullptr;
    ASSERT_EQ(seekdb_query(c1, alter, (int64_t)std::strlen(alter), &r), SEEKDB_SUCCESS)
        << "ALTER SYSTEM SET memory_limit failed";
    if (r)
        seekdb_result_free(r);

    // Let the server apply + persist the change before we shut it down.
    std::this_thread::sleep_for(5s);
    const std::string changed = read_parameter(c1, "memory_limit");
    ASSERT_FALSE(changed.empty()) << "could not read memory_limit after ALTER";
    ASSERT_NE(changed, seeded) << "ALTER SYSTEM SET did not change memory_limit (still '" << seeded
                               << "')";

    seekdb_disconnect(c1);

    // --- full shutdown so the next open must re-spawn (the restart path) ---
    shutdown_server(h1, pid1);

    // --- restart: store/sstable is now non-empty, so the driver must NOT pass
    //     --parameter, and the persisted value must remain ---
    SeekdbHandle h2 = nullptr;
    ASSERT_EQ(seekdb_open(db_dir_.c_str(), NULL, &h2), SEEKDB_SUCCESS);
    ASSERT_NE(h2, nullptr);
    const int64_t pid2 = ((SeekdbHandleImpl *)h2)->spawned_pid;
    ASSERT_GT(pid2, 0) << "restart should have re-spawned a server (not fast-path)";

    SeekdbConnection c2 = nullptr;
    ASSERT_EQ(seekdb_connect(h2, nullptr, true, &c2), SEEKDB_SUCCESS);

    const std::string after_restart = read_parameter(c2, "memory_limit");
    EXPECT_EQ(after_restart, changed)
        << "memory_limit changed across restart: persisted '" << changed << "' but read '"
        << after_restart << "' after restart";
    EXPECT_NE(after_restart, seeded)
        << "memory_limit was reset to the seeded default '" << seeded
        << "' on restart — driver clobbered the persisted value (issue #26)";

    seekdb_disconnect(c2);
    shutdown_server(h2, pid2);
}

} // namespace
