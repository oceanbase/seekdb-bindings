#include <gtest/gtest.h>

#include "port.h"

#include <cerrno>
#include <cstdlib>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

TEST(ProcessSpawn, StartsNewSession)
{
    char bin[] = "/bin/sleep";
    char duration[] = "30";
    char *argv[] = {bin, duration, nullptr};
    Process *process = nullptr;

    ASSERT_EQ(spawn_process(bin, argv, &process), OK);
    ASSERT_NE(process, nullptr);

    const pid_t pid = static_cast<pid_t>(process->pid);
    EXPECT_EQ(getsid(pid), pid);
    EXPECT_NE(getsid(pid), getsid(0));
    EXPECT_EQ(getpgid(pid), pid);
    EXPECT_NE(getpgid(pid), getpgrp());

    EXPECT_EQ(kill(pid, SIGKILL), 0);
    int status = 0;
    pid_t wait_result;
    do {
        wait_result = waitpid(pid, &status, 0);
    } while (wait_result == -1 && errno == EINTR);
    EXPECT_EQ(wait_result, pid);
    EXPECT_TRUE(WIFSIGNALED(status));

    free(process);
}

} // namespace
