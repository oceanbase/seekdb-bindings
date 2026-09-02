#include "seekdb.h"
#include "seekdb_internal.h"
#include "port.h"
#include "tlog.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#include <mysql.h>

#define WAIT_INTERVAL_US (200 * 1000)   /* 200 ms between discovery polls */
#define REAPER_INTERVAL_US (500 * 1000) /* 500 ms between reaper wakeups */
#define READY_TIMEOUT_MS (180ULL * 1000ULL)
#define PROBE_IO_TIMEOUT_SECONDS 1U
#define PROBE_PHASE_BUDGET_MS (10ULL * 1000ULL)
#define PROBE_FULL_BUDGET_MS (2ULL * PROBE_PHASE_BUDGET_MS)

#if defined(__GNUC__) || defined(__clang__)
#define MAYBE_UNUSED __attribute__((unused))
#else
#define MAYBE_UNUSED
#endif

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
MAYBE_UNUSED
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
MAYBE_UNUSED
static void start_reaper(void) { pthread_once(&g_reaper_once, start_reaper_once_cb); }
#endif

static bool monotonic_time_ms(uint64_t *out_ms)
{
    if (!out_ms)
        return false;
#ifdef _WIN32
    *out_ms = (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return false;
    *out_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
    return true;
}

static bool deadline_reached(uint64_t deadline_ms)
{
    uint64_t now_ms = 0;
    return !monotonic_time_ms(&now_ms) || now_ms >= deadline_ms;
}

static bool deadline_has_budget(uint64_t deadline_ms, uint64_t budget_ms)
{
    uint64_t now_ms = 0;
    if (!monotonic_time_ms(&now_ms))
        return false;
    return now_ms < deadline_ms && deadline_ms - now_ms >= budget_ms;
}

static void sleep_until_next_probe(uint64_t deadline_ms)
{
    uint64_t now_ms = 0;
    if (!monotonic_time_ms(&now_ms))
        return;
    if (now_ms >= deadline_ms)
        return;
    const uint64_t remaining_us = (deadline_ms - now_ms) * 1000ULL;
    const unsigned sleep_interval_us =
        remaining_us < WAIT_INTERVAL_US ? (unsigned)remaining_us : WAIT_INTERVAL_US;
    if (sleep_interval_us > 0)
        sleep_us(sleep_interval_us);
}

/* Acquire a lifecycle lock without allowing an OS-level blocking lock call to
 * escape the seekdb_open readiness deadline. The same 200 ms cadence is used
 * for locks and readiness probes. */
static int acquire_lock_until_deadline(Flock *lock, FlockMode mode, uint64_t deadline_ms,
                                       const char *lock_name)
{
    for (;;) {
        if (deadline_reached(deadline_ms)) {
            tlog("seekdb_open: timed out acquiring %s\n", lock_name);
            return 0;
        }
        if (flock_try_acquire(lock, mode))
            return 1;
        sleep_until_next_probe(deadline_ms);
    }
}

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
static void xfree(void *p)
{
    if (p)
        free(p);
}

static char *concat_strings(const char *left, const char *right)
{
    if (!left || !right)
        return NULL;
    const size_t left_len = strlen(left);
    const size_t right_len = strlen(right);
    if (left_len >= SIZE_MAX - right_len)
        return NULL;
    char *out = (char *)malloc(left_len + right_len + 1);
    if (!out)
        return NULL;
    memcpy(out, left, left_len);
    memcpy(out + left_len, right, right_len + 1);
    return out;
}

#ifndef _WIN32
static void cleanup_unix_socket_alias(SeekdbHandleImpl *h)
{
    if (!h || !h->socket_alias_dir)
        return;

    char run_link[128];
    const int n = snprintf(run_link, sizeof(run_link), "%s/run", h->socket_alias_dir);
    if (n >= 0 && (size_t)n < sizeof(run_link)) {
        if (unlink(run_link) != 0 && errno != ENOENT)
            tlog("cleanup_unix_socket_alias: unlink(%s) failed: errno=%d\n", run_link, errno);
    }
    else {
        tlog("cleanup_unix_socket_alias: alias path unexpectedly long: %s\n", h->socket_alias_dir);
    }

    if (rmdir(h->socket_alias_dir) != 0 && errno != ENOENT) {
        tlog("cleanup_unix_socket_alias: rmdir(%s) failed: errno=%d\n", h->socket_alias_dir, errno);
    }
    xfree(h->socket_alias_dir);
    h->socket_alias_dir = NULL;
}

static int prepare_unix_socket_alias(SeekdbHandleImpl *h, const char *run_dir)
{
    char *resolved_run_dir = realpath(run_dir, NULL);
    if (!resolved_run_dir) {
        tlog("prepare_unix_socket_alias: realpath(%s) failed: errno=%d\n", run_dir, errno);
        return SEEKDB_INTERNAL_ERROR;
    }

    char alias_template[128];
    const int template_n = snprintf(alias_template, sizeof(alias_template),
                                    "/tmp/pylibseekdb-uds-%lld-XXXXXX", (long long)getpid());
    if (template_n < 0 || (size_t)template_n >= sizeof(alias_template)) {
        tlog("prepare_unix_socket_alias: alias template truncated\n");
        xfree(resolved_run_dir);
        return SEEKDB_INTERNAL_ERROR;
    }

    if (!mkdtemp(alias_template)) {
        tlog("prepare_unix_socket_alias: mkdtemp(%s) failed: errno=%d\n", alias_template, errno);
        xfree(resolved_run_dir);
        return SEEKDB_INTERNAL_ERROR;
    }

    char *alias_dir = xstrdup(alias_template);
    char *run_link = concat_strings(alias_template, "/run");
    char *sock_path = concat_strings(alias_template, "/run/sql.sock");
    if (!alias_dir || !run_link || !sock_path) {
        tlog("prepare_unix_socket_alias: allocation failed\n");
        xfree(alias_dir);
        xfree(run_link);
        xfree(sock_path);
        rmdir(alias_template);
        xfree(resolved_run_dir);
        return SEEKDB_INTERNAL_ERROR;
    }

    if (symlink(resolved_run_dir, run_link) != 0) {
        tlog("prepare_unix_socket_alias: symlink(%s -> %s) failed: errno=%d\n", run_link,
             resolved_run_dir, errno);
        xfree(alias_dir);
        xfree(run_link);
        xfree(sock_path);
        rmdir(alias_template);
        xfree(resolved_run_dir);
        return SEEKDB_INTERNAL_ERROR;
    }

    h->socket_alias_dir = alias_dir;
    h->sock_path = sock_path;
    tlog("prepare_unix_socket_alias: %s -> %s (socket=%s)\n", run_link, resolved_run_dir,
         h->sock_path);

    xfree(run_link);
    xfree(resolved_run_dir);
    return SEEKDB_SUCCESS;
}
#endif

static void destroy_handle(SeekdbHandleImpl *h)
{
    if (!h)
        return;
    if (h->clients_lock) {
        flock_close(h->clients_lock);
        h->clients_lock = NULL;
    }
#ifndef _WIN32
    cleanup_unix_socket_alias(h);
#endif
    xfree(h->sock_path);
    xfree(h->clients_lock_path);
    xfree(h->startup_lock_path);
    xfree(h->db_dir);
    free(h);
}

/* ============================================================ utils ====== */

void *seekdb_malloc(size_t size) { return malloc(size); }
void seekdb_free(void *ptr) { xfree(ptr); }

/* ============================================================ handle ===== */

#ifdef _WIN32
/* Read the pipe-name suffix from <db_dir>/run/sql.pipe into h->pipe_name.
 * Returns 1 on success; 0 if the file is missing/empty (caller should retry). */
static int read_pipe_name(SeekdbHandleImpl *h)
{
    FILE *fp = fopen(h->pipe_file_path, "r");
    if (!fp) {
        tlog("read_pipe_name: fopen(%s) failed: errno=%d\n", h->pipe_file_path, errno);
        return 0;
    }
    char buf[256] = {0};
    char *r = fgets(buf, sizeof(buf), fp);
    fclose(fp);
    if (!r) {
        tlog("read_pipe_name: fgets(%s) returned NULL (empty file?)\n", h->pipe_file_path);
        return 0;
    }
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    if (n == 0) {
        tlog("read_pipe_name: %s contained only whitespace\n", h->pipe_file_path);
        return 0;
    }
    snprintf(h->pipe_name, sizeof(h->pipe_name), "%s", buf);
    int path_n = snprintf(h->pipe_path, sizeof(h->pipe_path), "\\\\.\\pipe\\%s", h->pipe_name);
    if (path_n < 0 || (size_t)path_n >= sizeof(h->pipe_path)) {
        h->pipe_path[0] = '\0';
        tlog("read_pipe_name: named-pipe path truncated: %s\n", h->pipe_name);
        return 0;
    }
    tlog("read_pipe_name: %s -> %s\n", h->pipe_file_path, h->pipe_path);
    return 1;
}
#endif

typedef enum {
    PROBE_LOCAL_NOT_FOUND = 0,
    PROBE_LOCAL_REACHED_UNVERIFIED = 1,
    PROBE_VERIFIED = 2,
} ProbeResult;

static void clear_discovered_server(SeekdbHandleImpl *h)
{
    h->host[0] = '\0';
    h->port = 0;
    h->server_uuid[0] = '\0';
}

static MYSQL *init_probe_mysql(void)
{
    MYSQL *m = mysql_init(NULL);
    if (!m)
        return NULL;

    /* Embedded seekdb's local and loopback listeners do not speak TLS. */
    char no_ssl = 0;
    unsigned int timeout_seconds = PROBE_IO_TIMEOUT_SECONDS;
    mysql_options(m, MYSQL_OPT_SSL_ENFORCE, &no_ssl);
    mysql_options(m, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &no_ssl);
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds);
    mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &timeout_seconds);
    mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &timeout_seconds);
    return m;
}

static int parse_discovered_port(const char *data, size_t len, unsigned int *out_port)
{
    if (!data || !out_port || len == 0 || len > 5)
        return 0;

    unsigned int port = 0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] < '0' || data[i] > '9')
            return 0;
        port = port * 10U + (unsigned int)(data[i] - '0');
    }
    if (port == 0 || port > 65535U)
        return 0;
    *out_port = port;
    return 1;
}

static int copy_discovered_uuid(const char *data, size_t len, char *out_uuid, size_t out_len)
{
    if (!data || !out_uuid || len == 0 || len >= out_len || memchr(data, '\0', len) != NULL)
        return 0;
    memcpy(out_uuid, data, len);
    out_uuid[len] = '\0';
    return 1;
}

/* Always discover readiness and identity through the db_dir-local endpoint.
 * A successful local mysql_real_connect means an instance owns this db_dir,
 * even if its discovery row is not ready or is malformed. */
static ProbeResult discover_local_server(SeekdbHandleImpl *h, unsigned int *out_port,
                                         char *out_uuid, size_t out_uuid_len)
{
    static const char discovery_sql[] =
        "SELECT SQL_PORT AS port, @@server_uuid AS server_uuid "
        "FROM oceanbase.V$OB_SERVER_STAT WHERE START_SERVICE_TIME > 0 LIMIT 1";

    MYSQL *m = init_probe_mysql();
    if (!m) {
        tlog("discover_local_server: mysql_init failed\n");
        return PROBE_LOCAL_REACHED_UNVERIFIED;
    }

#ifdef _WIN32
    if (!read_pipe_name(h)) {
        tlog("discover_local_server: %s not readable yet\n", h->pipe_file_path);
        mysql_close(m);
        return PROBE_LOCAL_NOT_FOUND;
    }
    mysql_options(m, MYSQL_OPT_NAMED_PIPE, NULL);
#endif

    if (!mysql_real_connect(m,
#ifdef _WIN32
                            ".",
#else
                            NULL,
#endif
                            "root", "", NULL, 0,
#ifdef _WIN32
                            h->pipe_name,
#else
                            h->sock_path,
#endif
                            0)) {
#ifdef _WIN32
        tlog("discover_local_server: db_dir=%s pipe=\\\\.\\pipe\\%s errno=%u: %s\n", h->db_dir,
             h->pipe_name, mysql_errno(m), mysql_error(m));
#else
        tlog("discover_local_server: db_dir=%s sock_path=%s errno=%u: %s\n", h->db_dir,
             h->sock_path, mysql_errno(m), mysql_error(m));
#endif
        mysql_close(m);
        return PROBE_LOCAL_NOT_FOUND;
    }

    ProbeResult result = PROBE_LOCAL_REACHED_UNVERIFIED;
    if (mysql_real_query(m, discovery_sql, (unsigned long)(sizeof(discovery_sql) - 1)) != 0) {
        tlog("discover_local_server: discovery query failed: %s\n", mysql_error(m));
        goto done;
    }

    MYSQL_RES *r = mysql_store_result(m);
    if (!r) {
        tlog("discover_local_server: store_result failed: %s\n", mysql_error(m));
        goto done;
    }

    if (mysql_num_fields(r) != 2 || mysql_num_rows(r) != 1) {
        tlog(
            "discover_local_server: expected exactly one two-column row, got fields=%u rows=%llu\n",
            mysql_num_fields(r), (unsigned long long)mysql_num_rows(r));
        mysql_free_result(r);
        goto done;
    }

    MYSQL_ROW row = mysql_fetch_row(r);
    unsigned long *lengths = row ? mysql_fetch_lengths(r) : NULL;
    if (!row || !lengths || !parse_discovered_port(row[0], (size_t)lengths[0], out_port) ||
        !copy_discovered_uuid(row[1], (size_t)lengths[1], out_uuid, out_uuid_len)) {
        tlog("discover_local_server: invalid port or empty/oversized server UUID\n");
        mysql_free_result(r);
        goto done;
    }

    result = PROBE_VERIFIED; /* locally discovered; TCP identity is checked by the caller */
    mysql_free_result(r);

done:
    mysql_close(m);
    return result;
}

static int verify_tcp_server(unsigned int port, const char *expected_uuid)
{
    static const char identity_sql[] = "SELECT @@server_uuid AS server_uuid";
    MYSQL *m = init_probe_mysql();
    if (!m) {
        tlog("verify_tcp_server: mysql_init failed\n");
        return 0;
    }

    int verified = 0;
    if (!mysql_real_connect(m, "127.0.0.1", "root", "", NULL, port, NULL, 0)) {
        tlog("verify_tcp_server: 127.0.0.1:%u connect failed: errno=%u: %s\n", port, mysql_errno(m),
             mysql_error(m));
        goto done;
    }
    if (mysql_real_query(m, identity_sql, (unsigned long)(sizeof(identity_sql) - 1)) != 0) {
        tlog("verify_tcp_server: identity query failed: %s\n", mysql_error(m));
        goto done;
    }

    MYSQL_RES *r = mysql_store_result(m);
    if (!r) {
        tlog("verify_tcp_server: store_result failed: %s\n", mysql_error(m));
        goto done;
    }
    if (mysql_num_fields(r) == 1 && mysql_num_rows(r) == 1) {
        MYSQL_ROW row = mysql_fetch_row(r);
        unsigned long *lengths = row ? mysql_fetch_lengths(r) : NULL;
        const size_t expected_len = strlen(expected_uuid);
        if (row && lengths && row[0] && lengths[0] > 0 && (size_t)lengths[0] == expected_len &&
            memcmp(row[0], expected_uuid, expected_len) == 0) {
            verified = 1;
        }
        else {
            tlog("verify_tcp_server: server UUID is empty or does not match local discovery\n");
        }
    }
    else {
        tlog("verify_tcp_server: expected exactly one UUID row, got fields=%u rows=%llu\n",
             mysql_num_fields(r), (unsigned long long)mysql_num_rows(r));
    }
    mysql_free_result(r);

done:
    mysql_close(m);
    return verified;
}

static ProbeResult probe_server(SeekdbHandleImpl *h, uint64_t deadline_ms)
{
    unsigned int candidate_port = 0;
    char candidate_uuid[sizeof(h->server_uuid)] = {0};
    clear_discovered_server(h);

    /* Connect, handshake and the scalar query use several separately timed
     * connector operations. Each has a one-second timeout; reserve a
     * conservative ten-second phase budget and never begin late I/O. */
    if (!deadline_has_budget(deadline_ms, PROBE_PHASE_BUDGET_MS)) {
        tlog("probe_server: insufficient deadline budget for local discovery\n");
        return PROBE_LOCAL_REACHED_UNVERIFIED;
    }

    ProbeResult result =
        discover_local_server(h, &candidate_port, candidate_uuid, sizeof(candidate_uuid));
    if (result != PROBE_VERIFIED)
        return result;
    if (!deadline_has_budget(deadline_ms, PROBE_PHASE_BUDGET_MS)) {
        clear_discovered_server(h);
        tlog("probe_server: insufficient deadline budget for TCP identity verification\n");
        return PROBE_LOCAL_REACHED_UNVERIFIED;
    }

    if (!verify_tcp_server(candidate_port, candidate_uuid)) {
        clear_discovered_server(h);
        return PROBE_LOCAL_REACHED_UNVERIFIED;
    }
    if (deadline_reached(deadline_ms)) {
        clear_discovered_server(h);
        return PROBE_LOCAL_REACHED_UNVERIFIED;
    }

    snprintf(h->host, sizeof(h->host), "%s", "127.0.0.1");
    h->port = (int)candidate_port;
    snprintf(h->server_uuid, sizeof(h->server_uuid), "%s", candidate_uuid);
    tlog("probe_server: verified server %s at %s:%d\n", h->server_uuid, h->host, h->port);
    return PROBE_VERIFIED;
}

/* Poll local discovery followed by TCP identity verification until the server
 * is ready, the process we spawned exits, or the shared deadline expires.
 * Returns 0 on readiness, -1 when spawned was reaped/freed, and -2 on timeout. */
static int wait_for_ready(SeekdbHandleImpl *h, Process *spawned, uint64_t deadline_ms)
{
    for (;;) {
        if (!deadline_has_budget(deadline_ms, PROBE_FULL_BUDGET_MS)) {
            clear_discovered_server(h);
            tlog("wait_for_ready: no deadline budget remains for another bounded probe\n");
            return -2;
        }

        const ProbeResult probe = probe_server(h, deadline_ms);
        if (deadline_reached(deadline_ms)) {
            clear_discovered_server(h);
            tlog("wait_for_ready: deadline reached during probe\n");
            return -2;
        }
        if (probe == PROBE_VERIFIED) {
            tlog("wait_for_ready: local discovery and TCP identity verification succeeded\n");
            return 0;
        }

        if (spawned && reap_process(spawned) == 1) {
            tlog("spawned %lld died\n", (long long)spawned->pid);
            free(spawned);
            return -1;
        }

        sleep_until_next_probe(deadline_ms);
    }
}

/* Resolve the seekdb server binary path as "<libseekdb's dir>/seekdb"
 * (".exe" suffix on Windows). The binary is expected to ship alongside the
 * shared library — that's the wheel/install layout. Writes the result into
 * `buf` (NUL-terminated). Returns SEEKDB_SUCCESS or SEEKDB_INTERNAL_ERROR. */
static int resolve_bin_path(char *buf, size_t buflen)
{
    char dir[1024];
    if (get_module_dir(dir, sizeof(dir)) != OK)
        return SEEKDB_INTERNAL_ERROR;
#ifdef _WIN32
    int n = snprintf(buf, buflen, "%s\\seekdb.exe", dir);
#else
    int n = snprintf(buf, buflen, "%s/seekdb", dir);
#endif
    if (n < 0 || (size_t)n >= buflen)
        return SEEKDB_INTERNAL_ERROR;
    return SEEKDB_SUCCESS;
}

static const char *default_parameters[] = {"memory_budget", "1G", "log_disk_size", "2G", NULL};

static int count_null_terminated(const char *const *arr)
{
    if (!arr)
        return 0;
    int n = 0;
    while (arr[n])
        n++;
    return n;
}

static bool is_driver_parameter(const char *key) { return key && strcmp(key, "port") == 0; }

static int validate_parameters(const char *const *parameters)
{
    if (!parameters)
        return SEEKDB_SUCCESS;
    const int n = count_null_terminated(parameters);
    if (n % 2 != 0)
        return SEEKDB_INVALID_ARGUMENT;
    for (int i = 0; i < n; i += 2) {
        if (!parameters[i] || !parameters[i + 1] || parameters[i][0] == '\0')
            return SEEKDB_INVALID_ARGUMENT;
        if (is_driver_parameter(parameters[i])) {
            errno = 0;
            char *endp = NULL;
            const long v = strtol(parameters[i + 1], &endp, 10);
            if (errno || !parameters[i + 1][0] || !endp || *endp != '\0' || v != 0)
                return SEEKDB_INVALID_ARGUMENT;
        }
    }
    return SEEKDB_SUCCESS;
}

static bool is_default_parameter_key(const char *key)
{
    const int n = count_null_terminated(default_parameters);
    for (int i = 0; i < n; i += 2) {
        if (strcmp(default_parameters[i], key) == 0)
            return true;
    }
    return false;
}

static const char *parameters_lookup(const char *const *parameters, const char *key)
{
    if (!parameters)
        return NULL;
    const int n = count_null_terminated(parameters);
    for (int i = 0; i < n; i += 2) {
        if (strcmp(parameters[i], key) == 0)
            return parameters[i + 1];
    }
    return NULL;
}

static int count_seed_pairs(const char *const *parameters)
{
    int count = count_null_terminated(default_parameters) / 2;
    if (!parameters)
        return count;
    const int n = count_null_terminated(parameters);
    for (int i = 0; i < n; i += 2) {
        if (is_driver_parameter(parameters[i]) || is_default_parameter_key(parameters[i]))
            continue;
        count++;
    }
    return count;
}

static int append_parameter_kv(char ***argv, int *argv_i, char **owned, size_t *owned_n,
                               const char *key, const char *val)
{
    const size_t kv_len = strlen(key) + strlen(val) + 2;
    char *kv = (char *)malloc(kv_len);
    if (!kv)
        return SEEKDB_INTERNAL_ERROR;
    snprintf(kv, kv_len, "%s=%s", key, val);
    owned[*owned_n] = kv;
    (*owned_n)++;
    (*argv)[(*argv_i)++] = (char *)"--parameter";
    (*argv)[(*argv_i)++] = kv;
    return SEEKDB_SUCCESS;
}

/* Build argv for spawn_process. Caller must free *out_argv and each *out_owned
 * entry. *out_argv is NULL-terminated. Driver-reserved keys are stripped and
 * default server parameters are merged with any user overrides/extras. */
static int build_spawn_argv(const char *bin_path, const char *base_dir_arg,
                            const char *const *parameters, bool first_init, char ***out_argv,
                            char ***out_owned, size_t *out_owned_n)
{
    *out_argv = NULL;
    *out_owned = NULL;
    *out_owned_n = 0;

    const int npairs = first_init ? count_seed_pairs(parameters) : 0;
    const int argc = 4 + (first_init ? npairs * 2 : 0) + 1;

    char **argv = (char **)calloc((size_t)argc, sizeof(char *));
    if (!argv)
        return SEEKDB_INTERNAL_ERROR;

    char **owned =
        first_init && npairs > 0 ? (char **)calloc((size_t)npairs, sizeof(char *)) : NULL;
    if (first_init && npairs > 0 && !owned) {
        free(argv);
        return SEEKDB_INTERNAL_ERROR;
    }

    int i = 0;
    argv[i++] = (char *)bin_path;
    argv[i++] = (char *)base_dir_arg;
    argv[i++] = (char *)"--embedded";
    argv[i++] = (char *)"--nodaemon";

    size_t owned_n = 0;
    if (first_init) {
        const int ndefaults = count_null_terminated(default_parameters) / 2;
        for (int p = 0; p < ndefaults; p++) {
            const char *key = default_parameters[p * 2];
            const char *val = parameters_lookup(parameters, key);
            if (!val)
                val = default_parameters[p * 2 + 1];
            if (append_parameter_kv(&argv, &i, owned, &owned_n, key, val) != SEEKDB_SUCCESS)
                goto fail;
        }

        if (parameters) {
            const int nentries = count_null_terminated(parameters);
            for (int si = 0; si < nentries; si += 2) {
                const char *key = parameters[si];
                const char *val = parameters[si + 1];
                if (is_driver_parameter(key) || is_default_parameter_key(key))
                    continue;
                if (append_parameter_kv(&argv, &i, owned, &owned_n, key, val) != SEEKDB_SUCCESS)
                    goto fail;
            }
        }
    }
    argv[i] = NULL;

    *out_argv = argv;
    *out_owned = owned;
    *out_owned_n = owned_n;
    return SEEKDB_SUCCESS;

fail:
    for (size_t j = 0; j < owned_n; j++)
        free(owned[j]);
    free(owned);
    free(argv);
    return SEEKDB_INTERNAL_ERROR;
}

static void free_spawn_argv(char **argv, char **owned, size_t owned_n)
{
    for (size_t i = 0; i < owned_n; i++)
        free(owned[i]);
    free(owned);
    free(argv);
}

int seekdb_open(const char *db_dir, const char **parameters, SeekdbHandle *out_handle)
{
    if (!db_dir || !out_handle)
        return SEEKDB_INVALID_ARGUMENT;
    const int param_rc = validate_parameters(parameters);
    if (param_rc != SEEKDB_SUCCESS)
        return param_rc;
    *out_handle = NULL;

    char bin_path[1024];
    if (resolve_bin_path(bin_path, sizeof(bin_path)) != SEEKDB_SUCCESS) {
        tlog("seekdb_open: cannot resolve seekdb binary "
             "(set SEEKDB_BIN or place seekdb next to libseekdb)\n");
        return SEEKDB_INTERNAL_ERROR;
    }

    tlog("seekdb_open: bin=%s db_dir=%s\n", bin_path, db_dir);

    int result = SEEKDB_INTERNAL_ERROR;
    SeekdbHandleImpl *h = (SeekdbHandleImpl *)calloc(1, sizeof(*h));
    char *run_dir = NULL;
    char *base_dir_arg = NULL;
    char *sstable_dir = NULL;
    Flock *startup_lock = NULL;
    Process *spawned = NULL;
    char **spawn_argv = NULL;
    char **spawn_owned = NULL;
    size_t spawn_owned_n = 0;
    bool first_init = false;
    int argv_rc = SEEKDB_INTERNAL_ERROR;
    int64_t spawned_pid = 0;
    int wait_rc = -1;
    uint64_t readiness_deadline_ms = 0;
    ProbeResult probe = PROBE_LOCAL_NOT_FOUND;
#ifdef _WIN32
    int pipe_n = 0;
#endif

    if (!h)
        goto cleanup;
    h->db_dir = xstrdup(db_dir);
    run_dir = concat_strings(db_dir, "/run");
    h->clients_lock_path = concat_strings(db_dir, "/run/seekdb.clients");
    h->startup_lock_path = concat_strings(db_dir, "/run/seekdb.startup");
    if (!h->db_dir || !run_dir || !h->clients_lock_path || !h->startup_lock_path) {
        tlog("seekdb_open: allocation failed while building paths for db_dir=%s\n", db_dir);
        goto cleanup;
    }
#ifdef _WIN32
    pipe_n = snprintf(h->pipe_file_path, sizeof(h->pipe_file_path), "%s/run/sql.pipe", db_dir);
    if (pipe_n < 0 || (size_t)pipe_n >= sizeof(h->pipe_file_path)) {
        tlog("seekdb_open: pipe discovery path truncated for db_dir=%s\n", db_dir);
        goto cleanup;
    }
#endif

    if (ensure_dir(db_dir) != OK) {
        tlog("ensure_dir failed: %s\n", db_dir);
        goto cleanup;
    }

    if (ensure_dir(run_dir) != OK) {
        tlog("ensure_dir failed: %s\n", run_dir);
        goto cleanup;
    }

#ifndef _WIN32
    if (prepare_unix_socket_alias(h, run_dir) != SEEKDB_SUCCESS)
        goto cleanup;
#endif

    if (flock_open(h->clients_lock_path, &h->clients_lock) != OK) {
        tlog("flock_open failed: %s\n", h->clients_lock_path);
        goto cleanup;
    }

    /* One shared deadline covers both lifecycle-lock waits, both local probes,
     * process startup and TCP identity verification. */
    uint64_t now_ms = 0;
    if (!monotonic_time_ms(&now_ms)) {
        tlog("seekdb_open: failed to read monotonic clock\n");
        goto cleanup;
    }
    readiness_deadline_ms = now_ms + READY_TIMEOUT_MS;
    if (!acquire_lock_until_deadline(h->clients_lock, FLOCK_SHARED, readiness_deadline_ms,
                                     "seekdb.clients SH")) {
        goto cleanup;
    }
    tlog("got seekdb.clients\n");

    probe = probe_server(h, readiness_deadline_ms);
    if (probe == PROBE_VERIFIED && !deadline_reached(readiness_deadline_ms)) {
        tlog("seekdb_open: fast-path success — server already serving\n");
        *out_handle = (SeekdbHandle)h;
        h = NULL;
        result = SEEKDB_SUCCESS;
        goto cleanup;
    }
    tlog("seekdb_open: initial local discovery did not verify a TCP endpoint\n");

    if (flock_open(h->startup_lock_path, &startup_lock) != OK) {
        tlog("flock_open failed: %s\n", h->startup_lock_path);
        goto cleanup;
    }
    if (!acquire_lock_until_deadline(startup_lock, FLOCK_EXCLUSIVE, readiness_deadline_ms,
                                     "seekdb.startup EX")) {
        goto cleanup;
    }
    tlog("got startup lock\n");

    /* The opener that held startup EX may have finished while we waited. Always
     * repeat local discovery after acquiring EX before deciding to spawn. */
    probe = probe_server(h, readiness_deadline_ms);
    if (probe == PROBE_VERIFIED && !deadline_reached(readiness_deadline_ms)) {
        tlog("seekdb_open: post-lock local discovery verified the existing server\n");
        *out_handle = (SeekdbHandle)h;
        h = NULL;
        result = SEEKDB_SUCCESS;
        goto cleanup;
    }
    if (deadline_reached(readiness_deadline_ms)) {
        clear_discovered_server(h);
        tlog("seekdb_open: readiness deadline reached after post-lock probe\n");
        goto cleanup;
    }

    if (probe == PROBE_LOCAL_NOT_FOUND) {
        base_dir_arg = concat_strings("--base-dir=", db_dir);
        sstable_dir = concat_strings(db_dir, "/store/sstable");
        if (!base_dir_arg || !sstable_dir) {
            tlog("seekdb_open: allocation failed while building spawn paths for db_dir=%s\n",
                 db_dir);
            goto cleanup;
        }

        /* seekdb writes its data files under <db_dir>/store/sstable only after it
         * has bootstrapped this data directory, so a non-empty store/sstable means
         * we're restarting an existing instance rather than initializing a fresh
         * one. We seed the default parameters (memory_budget, log_disk_size) ONLY on
         * first init; on restart we must not pass --parameter, otherwise the command
         * line would clobber values the user changed and seekdb persisted (issue #26).
         *
         * store/sstable is used (rather than store/ or etc/) because it is the core
         * storage-engine data directory and has been stable across seekdb versions,
         * whereas the etc/ layout (e.g. seekdb.config.bin) has changed between
         * releases. */
        first_init = !dir_has_entries(sstable_dir);
        tlog("seekdb_open: %s (sstable_dir=%s)\n",
             first_init ? "first init — seeding parameters"
                        : "restart — keeping persisted parameters",
             sstable_dir);

        argv_rc = build_spawn_argv(bin_path, base_dir_arg, parameters, first_init, &spawn_argv,
                                   &spawn_owned, &spawn_owned_n);
        if (argv_rc != SEEKDB_SUCCESS) {
            result = argv_rc;
            goto cleanup;
        }

        if (!deadline_has_budget(readiness_deadline_ms, PROBE_FULL_BUDGET_MS)) {
            tlog("seekdb_open: insufficient deadline budget to start a child\n");
            goto cleanup;
        }

        if (spawn_process(bin_path, spawn_argv, &spawned) != OK) {
            tlog("spawn process failed.");
            goto cleanup;
        }
        free_spawn_argv(spawn_argv, spawn_owned, spawn_owned_n);
        spawn_argv = NULL;
        spawn_owned = NULL;
        spawn_owned_n = 0;

        /* Mirror the Process fields onto the handle. wait_for_ready frees
         * `spawned` only when it reports that the process died. */
        h->spawned_pid = spawned->pid;
#ifdef _WIN32
        h->spawned_handle = spawned->handle;
#endif
        spawned_pid = spawned->pid;
        tlog("seekdb_open: spawned pid=%lld; waiting for verified TCP endpoint\n",
             (long long)spawned_pid);
    }
    else {
        /* A successful local connection proves a process already owns this
         * db_dir. Never start a second process merely because discovery data or
         * TCP identity verification is temporarily unavailable. */
        tlog("seekdb_open: local endpoint is owned; retrying discovery without spawning\n");
    }

    wait_rc = wait_for_ready(h, spawned, readiness_deadline_ms);

    tlog("seekdb_open: readiness wait finished; releasing startup lock\n");
    flock_close(startup_lock);
    startup_lock = NULL;

    if (wait_rc < 0) {
        if (wait_rc == -1) {
            spawned = NULL; /* wait_for_ready reaped and freed it */
        }
        else if (spawned) {
            /* Preserve the baseline process bookkeeping without adding timeout
             * termination or startup-lock ownership transfer. */
            spawned_add(spawned);
            spawned = NULL;
        }
        tlog("seekdb: server not ready\n");
        goto cleanup;
    }

    if (spawned) {
        /* Register the spawned process with the baseline background reaper
         * bookkeeping so it can be reaped once the server exits. */
        spawned_add(spawned);
        spawned = NULL;
        // start_reaper();
    }

    tlog("seekdb_open: success (spawned pid = %lld)\n", (long long)spawned_pid);
    *out_handle = (SeekdbHandle)h;
    h = NULL;
    result = SEEKDB_SUCCESS;

cleanup:
    free_spawn_argv(spawn_argv, spawn_owned, spawn_owned_n);
    if (startup_lock)
        flock_close(startup_lock);
    xfree(sstable_dir);
    xfree(base_dir_arg);
    xfree(run_dir);
    destroy_handle(h);
    return result;
}

int seekdb_close(SeekdbHandle handle)
{
    if (!handle)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbHandleImpl *h = (SeekdbHandleImpl *)handle;

    tlog("seekdb_close: db_dir=%s\n", h->db_dir);

    if (h->clients_lock)
        tlog("released seekdb.clients\n");
    destroy_handle(h);
    return SEEKDB_SUCCESS;
}

int seekdb_connection_options(SeekdbHandle handle, SeekdbConnectionOptions *out_options)
{
    if (!handle || !out_options)
        return SEEKDB_INVALID_ARGUMENT;

    SeekdbHandleImpl *h = (SeekdbHandleImpl *)handle;
    memset(out_options, 0, sizeof(*out_options));
    if (h->host[0] == '\0' || h->port <= 0 || h->port > 65535 || h->server_uuid[0] == '\0')
        return SEEKDB_INTERNAL_ERROR;

    out_options->transport = SEEKDB_CONNECTION_TRANSPORT_TCP;
    out_options->host = h->host;
    out_options->port = (unsigned int)h->port;
    out_options->user = "root";

    return SEEKDB_SUCCESS;
}

/* ======================================================= connection ===== */

int seekdb_connect(SeekdbHandle handle, const char *database, bool autocommit,
                   SeekdbConnection *out_connection)
{
    if (!handle || !out_connection)
        return SEEKDB_INVALID_ARGUMENT;
    *out_connection = NULL;

    SeekdbHandleImpl *h = (SeekdbHandleImpl *)handle;
    if (h->host[0] == '\0' || h->port <= 0 || h->port > 65535 || h->server_uuid[0] == '\0')
        return SEEKDB_INTERNAL_ERROR;

    tlog("seekdb_connect: tcp=%s:%d db=%s autocommit=%d\n", h->host, h->port,
         database ? database : "(null)", (int)autocommit);

    SeekdbConnectionImpl *c = (SeekdbConnectionImpl *)calloc(1, sizeof(*c));
    if (!c)
        return SEEKDB_INTERNAL_ERROR;

    c->mysql = mysql_init(NULL);
    if (!c->mysql) {
        free(c);
        return SEEKDB_INTERNAL_ERROR;
    }

    /* Disable SSL — see init_probe_mysql for the rationale. */
    {
        char no_ssl = 0;
        mysql_options(c->mysql, MYSQL_OPT_SSL_ENFORCE, &no_ssl);
        mysql_options(c->mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &no_ssl);
    }
    mysql_options(c->mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (!mysql_real_connect(c->mysql, h->host, "root", "", database, (unsigned int)h->port, NULL,
                            0)) {
        tlog("seekdb_connect failed: %s:%d: %s\n", h->host, h->port, mysql_error(c->mysql));
        *out_connection = (SeekdbConnection)c;
        return SEEKDB_INTERNAL_ERROR;
    }

    if (!autocommit) {
        if (mysql_real_query(c->mysql, "SET autocommit=0", 16)) {
            *out_connection = (SeekdbConnection)c;
            return SEEKDB_INTERNAL_ERROR;
        }
    }

    tlog("seekdb_connect: success\n");
    *out_connection = (SeekdbConnection)c;
    return SEEKDB_SUCCESS;
}

int seekdb_disconnect(SeekdbConnection connection)
{
    if (!connection)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbConnectionImpl *c = (SeekdbConnectionImpl *)connection;
    if (c->mysql)
        mysql_close(c->mysql);
    free(c);
    return SEEKDB_SUCCESS;
}

int seekdb_last_error(SeekdbConnection connection, int *out_errno, const char **out_msg)
{
    if (!connection)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbConnectionImpl *c = (SeekdbConnectionImpl *)connection;
    if (out_errno)
        *out_errno = c->mysql ? (int)mysql_errno(c->mysql) : 0;
    if (out_msg)
        *out_msg = c->mysql ? mysql_error(c->mysql) : "";
    return SEEKDB_SUCCESS;
}

/* ======================================================= transactions == */

static int run_simple(SeekdbConnectionImpl *c, const char *sql, size_t len)
{
    if (mysql_real_query(c->mysql, sql, (unsigned long)len))
        return SEEKDB_INTERNAL_ERROR;
    return SEEKDB_SUCCESS;
}

int seekdb_trx_begin(SeekdbConnection connection)
{
    if (!connection)
        return SEEKDB_INVALID_ARGUMENT;
    return run_simple((SeekdbConnectionImpl *)connection, "START TRANSACTION", 17);
}

int seekdb_trx_commit(SeekdbConnection connection)
{
    if (!connection)
        return SEEKDB_INVALID_ARGUMENT;
    return run_simple((SeekdbConnectionImpl *)connection, "COMMIT", 6);
}

int seekdb_trx_rollback(SeekdbConnection connection)
{
    if (!connection)
        return SEEKDB_INVALID_ARGUMENT;
    return run_simple((SeekdbConnectionImpl *)connection, "ROLLBACK", 8);
}

/* ============================================================ query ===== */

static SeekdbTypeId map_field_type(const MYSQL_FIELD *f)
{
    const bool is_unsigned = (f->flags & UNSIGNED_FLAG) != 0;
    switch (f->type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_YEAR:
        return is_unsigned ? SEEKDB_TYPE_UINT64 : SEEKDB_TYPE_INT64;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
        return SEEKDB_TYPE_FLOAT;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
        return SEEKDB_TYPE_DECIMAL;
    case MYSQL_TYPE_DATE:
        return SEEKDB_TYPE_DATE;
    case MYSQL_TYPE_DATETIME:
        return SEEKDB_TYPE_DATETIME;
    case MYSQL_TYPE_TIMESTAMP:
        return SEEKDB_TYPE_TIMESTAMP;
    case MYSQL_TYPE_NULL:
        return SEEKDB_TYPE_NULL;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
        return SEEKDB_TYPE_VARCHAR;
    default:
        return SEEKDB_TYPE_VARCHAR;
    }
}

int seekdb_query(SeekdbConnection connection, const char *sql, int64_t sql_len,
                 SeekdbResult *out_result)
{
    if (!connection || !sql || !out_result)
        return SEEKDB_INVALID_ARGUMENT;
    *out_result = NULL;

    SeekdbConnectionImpl *c = (SeekdbConnectionImpl *)connection;
    if (mysql_real_query(c->mysql, sql, (unsigned long)sql_len))
        return SEEKDB_INTERNAL_ERROR;

    MYSQL_RES *res = mysql_store_result(c->mysql);
    if (!res) {
        if (mysql_field_count(c->mysql) == 0) {
            /* OK with no result set (INSERT/UPDATE/DDL). */
        }
        else {
            return SEEKDB_INTERNAL_ERROR;
        }
    }

    SeekdbResultImpl *r = (SeekdbResultImpl *)calloc(1, sizeof(*r));
    if (!r) {
        if (res)
            mysql_free_result(res);
        return SEEKDB_INTERNAL_ERROR;
    }

    r->mysql = c->mysql;
    r->mysql_res = res;
    r->column_count = res ? (int)mysql_num_fields(res) : 0;

    *out_result = (SeekdbResult)r;
    return SEEKDB_SUCCESS;
}

/* =========================================================== result ==== */

int seekdb_result_free(SeekdbResult result)
{
    if (!result)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbResultImpl *r = (SeekdbResultImpl *)result;
    if (r->mysql_res)
        mysql_free_result(r->mysql_res);
    free(r);
    return SEEKDB_SUCCESS;
}

int seekdb_result_column_count(SeekdbResult result, int64_t *out_ncolumn)
{
    if (!result || !out_ncolumn)
        return SEEKDB_INVALID_ARGUMENT;
    *out_ncolumn = ((SeekdbResultImpl *)result)->column_count;
    return SEEKDB_SUCCESS;
}

int seekdb_result_column_name(SeekdbResult result, int64_t index, const char **out_name)
{
    if (!result || !out_name)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbResultImpl *r = (SeekdbResultImpl *)result;
    if (index < 0 || index >= r->column_count)
        return SEEKDB_INVALID_ARGUMENT;

    MYSQL_FIELD *f = mysql_fetch_field_direct(r->mysql_res, (unsigned int)index);
    if (!f)
        return SEEKDB_INTERNAL_ERROR;
    *out_name = f->name;
    return SEEKDB_SUCCESS;
}

int seekdb_result_column_type_id(SeekdbResult result, int64_t index, SeekdbTypeId *out_typeid)
{
    if (!result || !out_typeid)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbResultImpl *r = (SeekdbResultImpl *)result;
    if (index < 0 || index >= r->column_count)
        return SEEKDB_INVALID_ARGUMENT;

    MYSQL_FIELD *f = mysql_fetch_field_direct(r->mysql_res, (unsigned int)index);
    if (!f)
        return SEEKDB_INTERNAL_ERROR;
    *out_typeid = map_field_type(f);
    return SEEKDB_SUCCESS;
}

int seekdb_result_row_count(SeekdbResult result, int64_t *out_nrows)
{
    if (!result || !out_nrows)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbResultImpl *r = (SeekdbResultImpl *)result;
    *out_nrows = r->mysql_res ? (int64_t)mysql_num_rows(r->mysql_res) : 0;
    return SEEKDB_SUCCESS;
}

int seekdb_result_next(SeekdbResult result)
{
    if (!result)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbResultImpl *r = (SeekdbResultImpl *)result;
    if (!r->mysql_res)
        return SEEKDB_INTERNAL_ERROR;
    r->current_row = mysql_fetch_row(r->mysql_res);
    if (!r->current_row) {
        /* NULL from mysql_fetch_row means either end-of-result or an actual
         * fetch error. mysql_errno on the parent connection distinguishes. */
        r->current_lengths = NULL;
        return (mysql_errno(r->mysql) == 0) ? SEEKDB_NO_MORE_ROWS : SEEKDB_INTERNAL_ERROR;
    }
    r->current_lengths = mysql_fetch_lengths(r->mysql_res);
    return SEEKDB_SUCCESS;
}

int seekdb_result_get_int64(SeekdbResult result, int64_t index, int64_t *out_value)
{
    if (!result || !out_value)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbResultImpl *r = (SeekdbResultImpl *)result;
    if (index < 0 || index >= r->column_count)
        return SEEKDB_INVALID_ARGUMENT;
    if (!r->current_row)
        return SEEKDB_INTERNAL_ERROR;

    const char *data = r->current_row[index];
    if (!data) {
        *out_value = 0;
        return SEEKDB_SUCCESS;
    }
    size_t len = r->current_lengths[index];

    char buf[32];
    if (len >= sizeof(buf))
        return SEEKDB_INTERNAL_ERROR;
    memcpy(buf, data, len);
    buf[len] = '\0';
    errno = 0;
    char *endp = NULL;
    long long v = strtoll(buf, &endp, 10);
    if (errno || endp == buf)
        return SEEKDB_INTERNAL_ERROR;
    *out_value = (int64_t)v;
    return SEEKDB_SUCCESS;
}

int seekdb_result_get_uint64(SeekdbResult result, int64_t index, uint64_t *out_value)
{
    if (!result || !out_value)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbResultImpl *r = (SeekdbResultImpl *)result;
    if (index < 0 || index >= r->column_count)
        return SEEKDB_INVALID_ARGUMENT;
    if (!r->current_row)
        return SEEKDB_INTERNAL_ERROR;

    const char *data = r->current_row[index];
    if (!data) {
        *out_value = 0;
        return SEEKDB_SUCCESS;
    }
    size_t len = r->current_lengths[index];

    char buf[32];
    if (len >= sizeof(buf))
        return SEEKDB_INTERNAL_ERROR;
    memcpy(buf, data, len);
    buf[len] = '\0';
    errno = 0;
    char *endp = NULL;
    unsigned long long v = strtoull(buf, &endp, 10);
    if (errno || endp == buf)
        return SEEKDB_INTERNAL_ERROR;
    *out_value = (uint64_t)v;
    return SEEKDB_SUCCESS;
}

int seekdb_result_get_float(SeekdbResult result, int64_t index, double *out_value)
{
    if (!result || !out_value)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbResultImpl *r = (SeekdbResultImpl *)result;
    if (index < 0 || index >= r->column_count)
        return SEEKDB_INVALID_ARGUMENT;
    if (!r->current_row)
        return SEEKDB_INTERNAL_ERROR;

    const char *data = r->current_row[index];
    if (!data) {
        *out_value = 0.0;
        return SEEKDB_SUCCESS;
    }
    size_t len = r->current_lengths[index];

    char buf[64];
    if (len >= sizeof(buf))
        return SEEKDB_INTERNAL_ERROR;
    memcpy(buf, data, len);
    buf[len] = '\0';
    errno = 0;
    char *endp = NULL;
    double v = strtod(buf, &endp);
    if (errno || endp == buf)
        return SEEKDB_INTERNAL_ERROR;
    *out_value = v;
    return SEEKDB_SUCCESS;
}

int seekdb_result_get_str(SeekdbResult result, int64_t index, const char **out_data,
                          size_t *out_len, int *out_is_null)
{
    if (!result || !out_data || !out_len || !out_is_null)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbResultImpl *r = (SeekdbResultImpl *)result;
    if (index < 0 || index >= r->column_count)
        return SEEKDB_INVALID_ARGUMENT;
    if (!r->current_row)
        return SEEKDB_INTERNAL_ERROR;

    const char *cell = r->current_row[index];
    *out_is_null = (cell == NULL);
    *out_data = cell;
    *out_len = cell ? r->current_lengths[index] : 0;
    return SEEKDB_SUCCESS;
}

/* ============================================================ value ==== */

int seekdb_value_free(SeekdbValue value)
{
    if (!value)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbValueImpl *v = (SeekdbValueImpl *)value;
    if (v->type == SEEKDB_TYPE_VARCHAR || v->type == SEEKDB_TYPE_DECIMAL ||
        v->type == SEEKDB_TYPE_DATE || v->type == SEEKDB_TYPE_DATETIME ||
        v->type == SEEKDB_TYPE_TIMESTAMP) {
        xfree(v->v.str.data);
    }
    free(v);
    return SEEKDB_SUCCESS;
}

int seekdb_value_create_int64(int64_t int_value, SeekdbValue *out_value)
{
    if (!out_value)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbValueImpl *v = (SeekdbValueImpl *)calloc(1, sizeof(*v));
    if (!v)
        return SEEKDB_INTERNAL_ERROR;
    v->type = SEEKDB_TYPE_INT64;
    v->v.i64 = int_value;
    *out_value = (SeekdbValue)v;
    return SEEKDB_SUCCESS;
}

int seekdb_value_get_int64(SeekdbValue value, int64_t *out_value)
{
    if (!value || !out_value)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbValueImpl *v = (SeekdbValueImpl *)value;
    if (v->type != SEEKDB_TYPE_INT64)
        return SEEKDB_INVALID_ARGUMENT;
    *out_value = v->v.i64;
    return SEEKDB_SUCCESS;
}
