#include "seekdb_lifecycle.h"
#include "tlog.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define REAPER_INTERVAL_US (500 * 1000)

static char *g_binary_path = NULL;

/* ============================================================ reaper ====== */

typedef struct ProcessNode {
    Process *proc;
    struct ProcessNode *next;
} ProcessNode;

static ProcessNode *g_spawned = NULL;

#ifdef _WIN32

static CRITICAL_SECTION g_spawned_mu;
static INIT_ONCE g_mu_init = INIT_ONCE_STATIC_INIT;
static INIT_ONCE g_reaper_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_mu_cb(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o;
    (void)p;
    (void)c;
    InitializeCriticalSection(&g_spawned_mu);
    return TRUE;
}
static void lock_spawned(void)
{
    InitOnceExecuteOnce(&g_mu_init, init_mu_cb, NULL, NULL);
    EnterCriticalSection(&g_spawned_mu);
}
static void unlock_spawned(void) { LeaveCriticalSection(&g_spawned_mu); }
static void sleep_us(unsigned us) { Sleep(us / 1000); }

#else /* POSIX */

static pthread_mutex_t g_spawned_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_reaper_once = PTHREAD_ONCE_INIT;

static void lock_spawned(void) { pthread_mutex_lock(&g_spawned_mu); }
static void unlock_spawned(void) { pthread_mutex_unlock(&g_spawned_mu); }
static void sleep_us(unsigned us) { usleep(us); }

#endif

static void start_reaper(void);

static void spawned_add(Process *proc)
{
    ProcessNode *node = (ProcessNode *)malloc(sizeof(*node));
    if (!node)
        return;
    node->proc = proc;
    lock_spawned();
    node->next = g_spawned;
    g_spawned = node;
    unlock_spawned();
    start_reaper();
}

/* Background thread that wait_nonblocks each spawned server once it
 * exits, preventing zombies. Started lazily on the first successful spawn. */
static void reaper_loop_body(void)
{
    for (;;) {
        lock_spawned();
        ProcessNode **pp = &g_spawned;
        while (*pp) {
            ProcessNode *cur = *pp;
            if (reap_process(cur->proc) == 1) {
                tlog("reaper: reaped pid %lld\n", (long long)cur->proc->pid);
                free(cur->proc);
                *pp = cur->next;
                free(cur);
            }
            else {
                pp = &cur->next;
            }
        }
        unlock_spawned();
        sleep_us(REAPER_INTERVAL_US);
    }
}

#ifdef _WIN32
static DWORD WINAPI reaper_loop_win(LPVOID p)
{
    (void)p;
    reaper_loop_body();
    return 0;
}
static BOOL CALLBACK start_reaper_cb(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o;
    (void)p;
    (void)c;
    HANDLE h = CreateThread(NULL, 0, reaper_loop_win, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
    return TRUE;
}
static void start_reaper(void) { InitOnceExecuteOnce(&g_reaper_once, start_reaper_cb, NULL, NULL); }
#else
static void *reaper_loop_posix(void *arg)
{
    (void)arg;
    reaper_loop_body();
    return NULL;
}
static void start_reaper_once_cb(void)
{
    pthread_t tid;
    if (pthread_create(&tid, NULL, reaper_loop_posix, NULL) == 0) {
        pthread_detach(tid);
    }
}
static void start_reaper(void) { pthread_once(&g_reaper_once, start_reaper_once_cb); }
#endif

static char *xstrdup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

int seekdb_set_binary_path(const char *path)
{
    if (!path || path[0] == '\0')
        return SEEKDB_INVALID_ARGUMENT;
    char *copy = xstrdup(path);
    if (!copy)
        return SEEKDB_INTERNAL_ERROR;
    lock_spawned();
    free(g_binary_path);
    g_binary_path = copy;
    unlock_spawned();
    return SEEKDB_SUCCESS;
}
/* Resolve the seekdb server binary path as "<libseekdb's dir>/seekdb"
 * (".exe" suffix on Windows). The binary is expected to ship alongside the
 * shared library — that's the wheel/install layout. Writes the result into
 * `buf` (NUL-terminated). Returns SEEKDB_SUCCESS or SEEKDB_INTERNAL_ERROR. */
static int resolve_bin_path(char *buf, size_t buflen)
{
    lock_spawned();
    if (g_binary_path) {
        const int configured_n = snprintf(buf, buflen, "%s", g_binary_path);
        unlock_spawned();
        return configured_n >= 0 && (size_t)configured_n < buflen ? SEEKDB_SUCCESS
                                                                  : SEEKDB_INTERNAL_ERROR;
    }
    unlock_spawned();
    char dir[1024];
    if (get_module_dir(dir, sizeof(dir)) != OK)
        return SEEKDB_INTERNAL_ERROR;
#ifdef _WIN32
    int n = snprintf(buf, buflen, "%s\\seekdb.exe", dir);
    if (n < 0 || (size_t)n >= buflen) {
        return SEEKDB_INTERNAL_ERROR;
    }
#else
    /* Prefer the ordinary executable name. Android APKs may package the same
     * executable as libseekdb_exec.so so the installer extracts it into the
     * native-library directory, instead of app-writable data storage where
     * execution is restricted. It is still an executable, not a shared library.
     * Discover by layout rather than requiring an Android-specific branch.
     * Only fall back when missing: permission/exec failures must remain visible. */
    const char *names[] = {"seekdb", "libseekdb_exec.so"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        int n = snprintf(buf, buflen, "%s/%s", dir, names[i]);
        if (n < 0 || (size_t)n >= buflen) {
            return SEEKDB_INTERNAL_ERROR;
        }
        if (access(buf, F_OK) == 0 || errno != ENOENT) {
            return SEEKDB_SUCCESS;
        }
    }
    /* Leave the final candidate for the startup check to report its OS error. */
#endif
    return SEEKDB_SUCCESS;
}

int seekdb_open(const char *db_dir, const char **parameters, SeekdbHandle *out_handle)
{
    return seekdb_open_instance(db_dir, parameters, out_handle);
}

int seekdb_close(SeekdbHandle handle)
{
    if (!handle)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbHandleImpl *h = (SeekdbHandleImpl *)handle;

    tlog("seekdb_close: db_dir=%s\n", h->db_dir);

    if (h->clients_lock)
        tlog("released seekdb.clients\n");
    seekdb_destroy_handle(h);
    return SEEKDB_SUCCESS;
}

int seekdb_backend_binary_path(char *buf, size_t size, const char **out_path)
{
    if (resolve_bin_path(buf, size) != SEEKDB_SUCCESS) {
        seekdb_set_open_error("Cannot resolve SeekDB executable path; use seekdb_set_binary_path");
        return SEEKDB_INTERNAL_ERROR;
    }
    *out_path = buf;
    return SEEKDB_SUCCESS;
}

int seekdb_backend_check_endpoint(bool found, const char *db_dir) { return SEEKDB_SUCCESS; }

int seekdb_backend_start(SeekdbHandleImpl *h, const char *bin_path, char **argv, void **startup)
{
#ifndef _WIN32
    /* Android may report exec failure as a child exit after a successful spawn. */
    if (access(bin_path, X_OK) != 0) {
        int saved_errno = errno;
        seekdb_set_open_error("Cannot execute SeekDB binary %s: errno=%d (%s)", bin_path,
                              saved_errno, strerror(saved_errno));
        return SEEKDB_INTERNAL_ERROR;
    }
#endif
    Process *spawned = NULL;
    if (spawn_process(bin_path, argv, &spawned) != OK) {
#ifdef _WIN32
        seekdb_set_open_error(
            "Cannot spawn SeekDB executable %s; check executable and its dependencies", bin_path);
#else
        int saved_errno = errno;
        seekdb_set_open_error("Cannot spawn SeekDB executable %s: errno=%d (%s)", bin_path,
                              saved_errno, strerror(saved_errno));
#endif
        return SEEKDB_INTERNAL_ERROR;
    }
    h->spawned_pid = spawned->pid;
#ifdef _WIN32
    h->spawned_handle = spawned->handle;
#endif
    *startup = spawned;
    tlog("seekdb_open: spawned pid=%lld; waiting for server readiness\n", (long long)spawned->pid);
    return SEEKDB_SUCCESS;
}

int seekdb_backend_poll(void **startup)
{
    Process *spawned = (Process *)*startup;
    if (spawned && reap_process(spawned) == 1) {
        tlog("spawned %lld died\n", (long long)spawned->pid);
        free(spawned);
        *startup = NULL;
        return SEEKDB_INTERNAL_ERROR;
    }
    return SEEKDB_SUCCESS;
}

void seekdb_backend_finish(void *startup)
{
    /* Hand a live child to the background reaper on success or readiness timeout. */
    if (startup)
        spawned_add((Process *)startup);
}
