// Observation helpers shared across libseekdb tests.

#pragma once

#include "port.h"
#include "tlog.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef __linux__
#include <sys/stat.h>
#endif

// Build a unique db_dir path for the currently-running TEST_F, rooted under
// `root` (typically SEEKDB_TEST_DATA_ROOT). Combines suite + test name with a
// nanosecond timestamp so each invocation gets a fresh path — no fs::remove_all
// needed, which matters on Windows where files held open by the previous
// TEST_F's daemon (or by an orphan from a crashed prior run) can't be deleted.
inline std::string make_per_test_db_dir(const std::string &root)
{
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    return root + "/" + info->test_suite_name() + "." + info->name() + "_" + std::to_string(ns);
}
