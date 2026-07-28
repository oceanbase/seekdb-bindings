// Regression test for issue #44: seekdb_open accepts user-provided parameters
// on first init. Parameters are passed as a NULL-terminated key/value array and
// are only applied when store/sstable is empty (same first-init gate as #26).

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

class OpenParameters : public ::testing::Test {
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

TEST_F(OpenParameters, RejectsOddParameterCount)
{
    const char *bad[] = {"memory_limit", "10G", "only_key", NULL};
    SeekdbHandle h = nullptr;
    EXPECT_EQ(seekdb_open(db_dir_.c_str(), bad, &h), SEEKDB_INVALID_ARGUMENT);
    EXPECT_EQ(h, nullptr);
}

TEST_F(OpenParameters, RejectsInvalidPort)
{
    const char *bad[] = {"port", "not-a-number", NULL};
    SeekdbHandle h = nullptr;
    EXPECT_EQ(seekdb_open(db_dir_.c_str(), bad, &h), SEEKDB_INVALID_ARGUMENT);
    EXPECT_EQ(h, nullptr);
}

TEST_F(OpenParameters, PortOnlyStillSeedsDefaultServerParameters)
{
    const char *parameters[] = {"port", "0", NULL};

    SeekdbHandle h = nullptr;
    ASSERT_EQ(seekdb_open(db_dir_.c_str(), parameters, &h), SEEKDB_SUCCESS);
    ASSERT_NE(h, nullptr);
    const int64_t pid = ((SeekdbHandleImpl *)h)->spawned_pid;
    ASSERT_GT(pid, 0);

    SeekdbConnection c = nullptr;
    ASSERT_EQ(seekdb_connect(h, nullptr, true, &c), SEEKDB_SUCCESS);

    const std::string memory_limit = read_parameter(c, "memory_limit");
    ASSERT_FALSE(memory_limit.empty()) << "could not read memory_limit";
    EXPECT_NE(memory_limit.find("1G"), std::string::npos)
        << "expected default memory_limit=1G when only port is provided, got '" << memory_limit
        << "'";

    seekdb_disconnect(c);
    shutdown_server(h, pid);
}

TEST_F(OpenParameters, PartialUserParametersStillSeedDefaults)
{
    const char *parameters[] = {"memory_limit", "10G", NULL};

    SeekdbHandle h = nullptr;
    ASSERT_EQ(seekdb_open(db_dir_.c_str(), parameters, &h), SEEKDB_SUCCESS);
    ASSERT_NE(h, nullptr);
    const int64_t pid = ((SeekdbHandleImpl *)h)->spawned_pid;
    ASSERT_GT(pid, 0);

    SeekdbConnection c = nullptr;
    ASSERT_EQ(seekdb_connect(h, nullptr, true, &c), SEEKDB_SUCCESS);

    const std::string memory_limit = read_parameter(c, "memory_limit");
    ASSERT_FALSE(memory_limit.empty()) << "could not read memory_limit";
    EXPECT_NE(memory_limit.find("10G"), std::string::npos)
        << "expected user memory_limit=10G, got '" << memory_limit << "'";

    const std::string log_disk_size = read_parameter(c, "log_disk_size");
    ASSERT_FALSE(log_disk_size.empty()) << "could not read log_disk_size";
    EXPECT_NE(log_disk_size.find("2G"), std::string::npos)
        << "expected default log_disk_size=2G when omitted, got '" << log_disk_size << "'";

    seekdb_disconnect(c);
    shutdown_server(h, pid);
}

TEST_F(OpenParameters, SeedsUserProvidedParametersOnFirstInit)
{
    const char *parameters[] = {"memory_limit", "10G", "log_disk_size", "4G", NULL};

    SeekdbHandle h = nullptr;
    ASSERT_EQ(seekdb_open(db_dir_.c_str(), parameters, &h), SEEKDB_SUCCESS);
    ASSERT_NE(h, nullptr);
    const int64_t pid = ((SeekdbHandleImpl *)h)->spawned_pid;
    ASSERT_GT(pid, 0);

    SeekdbConnection c = nullptr;
    ASSERT_EQ(seekdb_connect(h, nullptr, true, &c), SEEKDB_SUCCESS);

    const std::string memory_limit = read_parameter(c, "memory_limit");
    ASSERT_FALSE(memory_limit.empty()) << "could not read memory_limit";
    EXPECT_NE(memory_limit.find("10G"), std::string::npos)
        << "expected memory_limit to include 10G, got '" << memory_limit << "'";

    const std::string log_disk_size = read_parameter(c, "log_disk_size");
    ASSERT_FALSE(log_disk_size.empty()) << "could not read log_disk_size";
    EXPECT_NE(log_disk_size.find("4G"), std::string::npos)
        << "expected log_disk_size to include 4G, got '" << log_disk_size << "'";

    seekdb_disconnect(c);
    shutdown_server(h, pid);
}

} // namespace
