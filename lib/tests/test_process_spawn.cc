#include <gtest/gtest.h>

#include "port.h"

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32

#include <windows.h>

#else

#include <cerrno>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#endif

namespace {

#ifndef _WIN32

volatile sig_atomic_t received_sigint = 0;

void record_sigint(int) { received_sigint = 1; }

void kill_and_reap(Process *process)
{
    if (process == nullptr)
        return;
    const pid_t pid = static_cast<pid_t>(process->pid);
    (void)kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    free(process);
}

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

TEST(ProcessSpawn, ParentGroupSigintDoesNotReachChild)
{
    if (getpgrp() != getpid())
        ASSERT_EQ(setpgid(0, 0), 0);
    ASSERT_EQ(getpgrp(), getpid());

    struct sigaction action = {};
    struct sigaction old_action = {};
    action.sa_handler = record_sigint;
    sigemptyset(&action.sa_mask);
    ASSERT_EQ(sigaction(SIGINT, &action, &old_action), 0);

    char bin[] = "/bin/sleep";
    char duration[] = "30";
    char *argv[] = {bin, duration, nullptr};
    Process *process = nullptr;
    ASSERT_EQ(spawn_process(bin, argv, &process), OK);
    ASSERT_NE(process, nullptr);

    received_sigint = 0;
    ASSERT_EQ(kill(-getpgrp(), SIGINT), 0);
    for (int i = 0; i < 100 && received_sigint == 0; ++i) {
        struct timespec delay = {0, 10 * 1000 * 1000};
        nanosleep(&delay, nullptr);
    }
    EXPECT_EQ(received_sigint, 1);

    int status = 0;
    const pid_t child = static_cast<pid_t>(process->pid);
    const pid_t wait_result = waitpid(child, &status, WNOHANG);
    EXPECT_EQ(wait_result, 0) << "child exited after SIGINT targeted its parent's process group";
    if (wait_result == 0)
        kill_and_reap(process);
    else
        free(process);

    ASSERT_EQ(sigaction(SIGINT, &old_action, nullptr), 0);
}

#else

volatile LONG received_control_break = 0;

BOOL WINAPI record_control_break(DWORD control_type)
{
    if (control_type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&received_control_break, 1);
        return TRUE;
    }
    return FALSE;
}

std::string current_executable()
{
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
        return {};
    return std::string(path, length);
}

int run_signal_child(const char *ready_event_name)
{
    if (!SetConsoleCtrlHandler(record_control_break, TRUE))
        return 10;
    HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, ready_event_name);
    if (ready == nullptr)
        return 11;
    const BOOL notified = SetEvent(ready);
    CloseHandle(ready);
    if (!notified)
        return 12;
    for (int i = 0; i < 300 && InterlockedCompareExchange(&received_control_break, 0, 0) == 0; ++i)
        Sleep(100);
    return InterlockedCompareExchange(&received_control_break, 0, 0) == 0 ? 0 : 13;
}

int run_isolated_parent()
{
    received_control_break = 0;
    if (!SetConsoleCtrlHandler(record_control_break, TRUE))
        return 20;

    const std::string executable = current_executable();
    if (executable.empty())
        return 21;

    char ready_event_name[128];
    snprintf(ready_event_name, sizeof(ready_event_name), "Local\\seekdb-spawn-ready-%lu-%llu",
             static_cast<unsigned long>(GetCurrentProcessId()),
             static_cast<unsigned long long>(GetTickCount64()));
    HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, ready_event_name);
    if (ready == nullptr)
        return 22;

    std::string executable_arg = executable;
    std::string mode_arg = "--signal-child";
    std::string event_arg = ready_event_name;
    char *argv[] = {executable_arg.data(), mode_arg.data(), event_arg.data(), nullptr};
    Process *process = nullptr;
    if (spawn_process(executable.c_str(), argv, &process) != OK || process == nullptr) {
        CloseHandle(ready);
        return 23;
    }

    int result = 0;
    if (WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0)
        result = 24;
    else if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, GetCurrentProcessId()))
        result = 25;
    else {
        for (int i = 0; i < 100 && InterlockedCompareExchange(&received_control_break, 0, 0) == 0;
             ++i)
            Sleep(10);
        if (InterlockedCompareExchange(&received_control_break, 0, 0) == 0)
            result = 26;
        else {
            Sleep(300);
            if (reap_process(process) != -1) {
                process = nullptr;
                result = 27;
            }
        }
    }

    if (process != nullptr) {
        const HANDLE child_handle = static_cast<HANDLE>(process->handle);
        (void)terminate_process(process->pid, 0);
        (void)WaitForSingleObject(child_handle, 5000);
        (void)reap_process(process);
        free(process);
    }
    CloseHandle(ready);
    return result;
}

TEST(ProcessSpawn, ParentConsoleGroupBreakDoesNotReachChild)
{
    bool allocated_console = false;
    if (GetConsoleCP() == 0) {
        ASSERT_TRUE(AllocConsole());
        allocated_console = true;
    }

    const std::string executable = current_executable();
    ASSERT_FALSE(executable.empty());
    std::string command_line = "\"" + executable + "\" --isolated-parent";
    STARTUPINFOA startup = {};
    PROCESS_INFORMATION process = {};
    startup.cb = sizeof(startup);
    ASSERT_TRUE(CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                               CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process));
    CloseHandle(process.hThread);

    ASSERT_EQ(WaitForSingleObject(process.hProcess, 15000), WAIT_OBJECT_0);
    DWORD exit_code = 0;
    ASSERT_TRUE(GetExitCodeProcess(process.hProcess, &exit_code));
    CloseHandle(process.hProcess);
    EXPECT_EQ(exit_code, 0U) << "isolated parent scenario failed with code " << exit_code;

    if (allocated_console)
        FreeConsole();
}

#endif

} // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
    if (argc == 3 && strcmp(argv[1], "--signal-child") == 0)
        return run_signal_child(argv[2]);
    if (argc == 2 && strcmp(argv[1], "--isolated-parent") == 0)
        return run_isolated_parent();
#endif
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
