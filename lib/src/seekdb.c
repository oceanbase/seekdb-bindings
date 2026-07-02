#include "seekdb.h"
#include "seekdb_internal.h"
#include "port.h"
#include "tlog.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#include <mysql.h>

#define WAIT_INTERVAL_US (200 * 1000)   /* 200 ms between try_connect polls */
#define REAPER_INTERVAL_US (500 * 1000) /* 500 ms between reaper wakeups */

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
    tlog("read_pipe_name: %s -> \\\\.\\pipe\\%s\n", h->pipe_file_path, h->pipe_name);
    return 1;
}
#endif

static int try_connect(SeekdbHandleImpl *h)
{

    MYSQL *m = mysql_init(NULL);
    if (!m)
        return 0;

    /* Disable TLS — seekdb's local listeners don't speak TLS. libmariadb 3.4
     * keeps MYSQL_OPT_SSL_ENFORCE for compat (marked deprecated, still honored). */
    char no_ssl = 0;
    mysql_options(m, MYSQL_OPT_SSL_ENFORCE, &no_ssl);
    mysql_options(m, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &no_ssl);
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    const bool use_tcp = h->port != 0;
#ifdef _WIN32
    if (!use_tcp) {
        if (!read_pipe_name(h)) {
            tlog("try_connect: %s not readable yet\n", h->pipe_file_path);
            mysql_close(m);
            return 0;
        }
        mysql_options(m, MYSQL_OPT_NAMED_PIPE, NULL);
    }
#endif
    if (mysql_real_connect(m,
                           use_tcp ? h->host :
#ifdef _WIN32
                                   ".",
#else
                                   NULL,
#endif
                           "root@sys", "", NULL, use_tcp ? (unsigned int)h->port : 0,
                           use_tcp ? NULL :
#ifdef _WIN32
                                   h->pipe_name,
#else
                                   h->sock_path,
#endif
                           0)) {
        if (mysql_real_query(m, "SELECT 1", 8) == 0) {
            MYSQL_RES *r = mysql_store_result(m);
            if (r) {
                mysql_free_result(r);
                tlog("try connected succeeded\n");
                mysql_close(m);
                return 1;
            }
            else {
                tlog("try_connect: store_result failed: %s\n", mysql_error(m));
            }
        }
        else {
            tlog("try_connect: SELECT 1 failed: %s\n", mysql_error(m));
        }
    }
    else {
        const unsigned int err = mysql_errno(m);
        const char *msg = mysql_error(m);
        if (use_tcp) {
            tlog("try_connect failed: db_dir=%s, host=%s:%d, errno=%u: %s\n", h->db_dir, h->host,
                 h->port, err, msg ? msg : "");
        }
        else {
#ifdef _WIN32
            tlog("try_connect failed: db_dir=%s, pipe=\\\\.\\pipe\\%s, errno=%u: %s\n", h->db_dir,
                 h->pipe_name, err, msg ? msg : "");
#else
            tlog("try_connect failed: db_dir=%s, sock_path=%s, errno=%u: %s\n", h->db_dir,
                 h->sock_path, err, msg ? msg : "");
#endif
        }
    }
    mysql_close(m);
    return 0;
    ;
}

/* Poll until the spawned server is ready to serve or has died.
 *   Returns  0: server is ready (try_connect succeeded).
 *   Returns -1: server exited before becoming ready; reap_process
 *               performed the kernel-side reap and we then freed the
 *               Process struct (caller's `spawned` pointer is dangling).
 *   Otherwise: keeps polling — sleeps WAIT_INTERVAL_US between attempts. */
static int wait_for_ready(SeekdbHandleImpl *h, Process *spawned)
{
    for (;;) {
        if (try_connect(h)) {
            tlog("wait_for_ready: try_connect succeeded\n");
            return 0;
        }
        tlog("wait_for_ready: cannot connect\n");

        /* Our spawned server died before becoming ready — stop waiting. */
        if (reap_process(spawned) == 1) {
            tlog("spawned %lld died\n", (long long)spawned->pid);
            free(spawned);
            return -1;
        }

        sleep_us(WAIT_INTERVAL_US);
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

int seekdb_open(const char *db_dir, int port, SeekdbHandle *out_handle)
{
    if (!db_dir || !out_handle)
        return SEEKDB_INVALID_ARGUMENT;
    *out_handle = NULL;

    char bin_path[1024];
    if (resolve_bin_path(bin_path, sizeof(bin_path)) != SEEKDB_SUCCESS) {
        tlog("seekdb_open: cannot resolve seekdb binary "
             "(set SEEKDB_BIN or place seekdb next to libseekdb)\n");
        return SEEKDB_INTERNAL_ERROR;
    }

    tlog("seekdb_open: bin=%s db_dir=%s\n", bin_path, db_dir);

    SeekdbHandleImpl *h = (SeekdbHandleImpl *)calloc(1, sizeof(*h));
    h->db_dir = xstrdup(db_dir);
    snprintf(h->sock_path, sizeof(h->sock_path), "%s/run/sql.sock", db_dir);
    snprintf(h->clients_lock_path, sizeof(h->clients_lock_path), "%s/run/seekdb.clients", db_dir);
    snprintf(h->startup_lock_path, sizeof(h->startup_lock_path), "%s/run/seekdb.startup", db_dir);
#ifdef _WIN32
    snprintf(h->pipe_file_path, sizeof(h->pipe_file_path), "%s/run/sql.pipe", db_dir);
#endif

    if (port != 0) {
        snprintf(h->host, sizeof(h->host), "127.0.0.1");
        h->port = port;
    }

    if (ensure_dir(db_dir) != OK) {
        tlog("ensure_dir failed: %s\n", db_dir);
        xfree(h->db_dir);
        free(h);
        return SEEKDB_INTERNAL_ERROR;
    }

    char run_dir[256];
    snprintf(run_dir, sizeof(run_dir), "%s/run", db_dir);
    if (ensure_dir(run_dir) != OK) {
        tlog("ensure_dir failed: %s\n", run_dir);
        xfree(h->db_dir);
        free(h);
        return SEEKDB_INTERNAL_ERROR;
    }

    if (flock_open(h->clients_lock_path, &h->clients_lock) != OK) {
        tlog("flock_open failed: %s\n", h->clients_lock_path);
        xfree(h->db_dir);
        free(h);
        return SEEKDB_INTERNAL_ERROR;
    }

    if (!flock_acquire(h->clients_lock, FLOCK_SHARED)) {
        tlog("flock_acquire(seekdb.clients, SH) failed\n");
        flock_close(h->clients_lock);
        xfree(h->db_dir);
        free(h);
        return SEEKDB_INTERNAL_ERROR;
    }
    tlog("got seekdb.clients\n");

    if (try_connect(h)) {
        tlog("seekdb_open: fast-path success — server already serving\n");
        *out_handle = (SeekdbHandle)h;
        return SEEKDB_SUCCESS;
    }
    tlog("tried to connect after getting client lock, but failed\n");

    Flock *startup_lock = NULL;
    if (flock_open(h->startup_lock_path, &startup_lock) != OK) {
        tlog("flock_open failed: %s\n", h->startup_lock_path);
        flock_close(h->clients_lock);
        xfree(h->db_dir);
        free(h);
        return SEEKDB_INTERNAL_ERROR;
    }
    if (!flock_acquire(startup_lock, FLOCK_EXCLUSIVE)) {
        tlog("flock_acquire(seekdb.startup, EX) failed\n");
        flock_close(startup_lock);
        flock_close(h->clients_lock);
        xfree(h->db_dir);
        free(h);
        return SEEKDB_INTERNAL_ERROR;
    }
    tlog("got startup lock\n");

    Process *spawned = NULL;
    char base_dir_arg[512];
    snprintf(base_dir_arg, sizeof(base_dir_arg), "--base-dir=%s", db_dir);

    /* seekdb writes its data files under <db_dir>/store/sstable only after it
     * has bootstrapped this data directory, so a non-empty store/sstable means
     * we're restarting an existing instance rather than initializing a fresh
     * one. We seed the default parameters (memory_limit, log_disk_size) ONLY on
     * first init; on restart we must not pass --parameter, otherwise the command
     * line would clobber values the user changed and seekdb persisted (issue #26).
     *
     * store/sstable is used (rather than store/ or etc/) because it is the core
     * storage-engine data directory and has been stable across seekdb versions,
     * whereas the etc/ layout (e.g. seekdb.config.bin) has changed between
     * releases. */
    char sstable_dir[512];
    int sstable_n = snprintf(sstable_dir, sizeof(sstable_dir), "%s/store/sstable", db_dir);
    bool first_init;
    if (sstable_n < 0 || (size_t)sstable_n >= sizeof(sstable_dir)) {
        /* Path truncated — we can't reliably probe store/sstable. Fall back to
         * the pre-#26 behavior (always seed defaults) so a genuine first init
         * can still bootstrap; unreachable in practice since the handle's other
         * path buffers (sock_path, 256 bytes) would have failed first. */
        tlog("seekdb_open: sstable path truncated for db_dir=%s — seeding defaults\n", db_dir);
        first_init = true;
    }
    else {
        first_init = !dir_has_entries(sstable_dir);
    }
    tlog("seekdb_open: %s (sstable_dir=%s)\n",
         first_init ? "first init — seeding default parameters"
                    : "restart — keeping persisted parameters",
         sstable_dir);

    char *first_init_argv[] = {(char *)bin_path,
                               base_dir_arg,
                               (char *)"--embedded",
                               (char *)"--nodaemon",
                               (char *)"--parameter",
                               (char *)"memory_limit=1G",
                               (char *)"--parameter",
                               (char *)"log_disk_size=2G",
                               NULL};
    char *restart_argv[] = {(char *)bin_path, base_dir_arg, (char *)"--embedded",
                            (char *)"--nodaemon", NULL};
    char *const *argv = first_init ? first_init_argv : restart_argv;

    if (spawn_process(bin_path, argv, &spawned) != OK) {
        flock_close(startup_lock);
        flock_close(h->clients_lock);
        xfree(h->db_dir);
        free(h);
        tlog("spawn process failed.");
        return SEEKDB_INTERNAL_ERROR;
    }

    /* Mirror the Process fields onto the handle. wait_for_ready frees
     * `spawned` on -1, so deref before the call. */
    h->spawned_pid = spawned->pid;
#ifdef _WIN32
    h->spawned_handle = spawned->handle;
#endif
    const int64_t spawned_pid = spawned->pid;
    tlog("ready to call wait_for_ready(spawned pid = %lld)\n", (long long)spawned_pid);
    int rc = wait_for_ready(h, spawned);

    tlog("spawned pid = %lld, released startup\n", (long long)spawned_pid);
    flock_close(startup_lock);

    if (rc < 0) {
        tlog("seekdb: server not ready\n");
        flock_close(h->clients_lock);
        xfree(h->db_dir);
        free(h);
        return SEEKDB_INTERNAL_ERROR;
    }

    /* Register the spawned process with the background reaper so it
     * gets reaped once the server exits. Start the reaper lazily. */
    spawned_add(spawned);
    // start_reaper();

    tlog("seekdb_open: success (spawned pid = %lld)\n", (long long)spawned_pid);
    *out_handle = (SeekdbHandle)h;
    return SEEKDB_SUCCESS;
}

int seekdb_close(SeekdbHandle handle)
{
    if (!handle)
        return SEEKDB_INVALID_ARGUMENT;
    SeekdbHandleImpl *h = (SeekdbHandleImpl *)handle;

    tlog("seekdb_close: db_dir=%s\n", h->db_dir);

    if (h->clients_lock) {
        flock_close(h->clients_lock);
        tlog("released seekdb.clients\n");
    }

    xfree(h->db_dir);
    free(h);
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

    const bool use_tcp = h->port != 0;
    if (use_tcp) {
        tlog("seekdb_connect: tcp=%s:%d db=%s autocommit=%d\n", h->host, h->port,
             database ? database : "(null)", (int)autocommit);
    }
    else {
#ifdef _WIN32
        tlog("seekdb_connect: pipe=\\\\.\\pipe\\%s db=%s autocommit=%d\n", h->pipe_name,
             database ? database : "(null)", (int)autocommit);
#else
        tlog("seekdb_connect: sock=%s db=%s autocommit=%d\n", h->sock_path,
             database ? database : "(null)", (int)autocommit);
#endif
    }

    SeekdbConnectionImpl *c = (SeekdbConnectionImpl *)calloc(1, sizeof(*c));
    if (!c)
        return SEEKDB_INTERNAL_ERROR;

    c->mysql = mysql_init(NULL);
    if (!c->mysql) {
        free(c);
        return SEEKDB_INTERNAL_ERROR;
    }

    /* Disable SSL — see try_connect for the rationale. */
    {
        char no_ssl = 0;
        mysql_options(c->mysql, MYSQL_OPT_SSL_ENFORCE, &no_ssl);
        mysql_options(c->mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &no_ssl);
    }
    mysql_options(c->mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");
#ifdef _WIN32
    if (!use_tcp) {
        mysql_options(c->mysql, MYSQL_OPT_NAMED_PIPE, NULL);
    }
#endif

    if (!mysql_real_connect(c->mysql,
                            use_tcp ? h->host :
#ifdef _WIN32
                                    ".",
#else
                                    NULL,
#endif
                            "root", "", database, use_tcp ? (unsigned int)h->port : 0,
                            use_tcp ? NULL :
#ifdef _WIN32
                                    h->pipe_name,
#else
                                    h->sock_path,
#endif
                            0)) {
        if (use_tcp) {
            tlog("seekdb_connect failed: %s:%d: %s\n", h->host, h->port, mysql_error(c->mysql));
        }
        else {
#ifdef _WIN32
            tlog("seekdb_connect failed: \\\\.\\pipe\\%s: %s\n", h->pipe_name,
                 mysql_error(c->mysql));
#else
            tlog("seekdb_connect failed: %s: %s\n", h->sock_path, mysql_error(c->mysql));
#endif
        }
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
