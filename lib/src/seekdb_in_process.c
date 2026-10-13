#include "seekdb_lifecycle.h"
#include "seekdb_engine.h"
#include "tlog.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static SeekdbEngineHandle g_engine = NULL;
static char *g_engine_dir = NULL;
static size_t g_engine_refs = 0;
#ifdef _WIN32
static SRWLOCK g_engine_mutex = SRWLOCK_INIT;
static void lock_engine(void) { AcquireSRWLockExclusive(&g_engine_mutex); }
static void unlock_engine(void) { ReleaseSRWLockExclusive(&g_engine_mutex); }
#else
static pthread_mutex_t g_engine_mutex = PTHREAD_MUTEX_INITIALIZER;
static void lock_engine(void) { pthread_mutex_lock(&g_engine_mutex); }
static void unlock_engine(void) { pthread_mutex_unlock(&g_engine_mutex); }
#endif

int seekdb_open(const char *db_dir, const char **parameters, SeekdbHandle *out_handle)
{
    seekdb_set_open_error("%s", "");
    if (out_handle)
        *out_handle = NULL;
    if (!db_dir || !db_dir[0] || !out_handle ||
        seekdb_validate_parameters(parameters) != SEEKDB_SUCCESS) {
        seekdb_set_open_error("Invalid database directory, output handle, or parameters");
        return SEEKDB_INVALID_ARGUMENT;
    }
    /* The engine changes process cwd. Require absolute paths even for later
     * opens so their meaning does not change during an engine lifetime. */
#ifdef _WIN32
    if (!((db_dir[0] && db_dir[1] == ':' && (db_dir[2] == '/' || db_dir[2] == '\\')) ||
          (db_dir[0] == '\\' && db_dir[1] == '\\'))) {
#else
    if (db_dir[0] != '/') {
#endif
        seekdb_set_open_error("In-process mode requires an absolute database directory");
        return SEEKDB_INVALID_ARGUMENT;
    }
    lock_engine();
    int result = SEEKDB_INTERNAL_ERROR;
    char *canonical = NULL;
    if (ensure_dir(db_dir) != OK) {
        seekdb_set_open_error("Cannot create/access database directory %s: %s", db_dir,
                              strerror(errno));
        goto done;
    }
#ifdef _WIN32
    canonical = _fullpath(NULL, db_dir, 0);
#else
    canonical = realpath(db_dir, NULL);
#endif
    if (!canonical) {
        seekdb_set_open_error("Cannot resolve database directory %s: %s", db_dir, strerror(errno));
        goto done;
    }
    if (g_engine && strcmp(canonical, g_engine_dir) != 0) {
        seekdb_set_open_error("Only one in-process database directory may be open at a time");
        result = SEEKDB_INVALID_ARGUMENT;
        goto done;
    }
    result = seekdb_open_instance(canonical, parameters, out_handle);
    if (result == SEEKDB_SUCCESS) {
        if (g_engine_refs == 0) {
            g_engine_dir = canonical;
            canonical = NULL;
        }
        ++g_engine_refs;
    }
    else if (g_engine && g_engine_refs == 0) {
        /* Startup may succeed before endpoint discovery fails. Never leave an
         * unowned engine running, and preserve the original diagnostic. */
        seekdb_engine_close(g_engine);
        g_engine = NULL;
    }
done:
    free(canonical);
    unlock_engine();
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
    lock_engine();
    seekdb_destroy_handle(h);
    int result = SEEKDB_SUCCESS;
    if (--g_engine_refs == 0) {
        const int engine_result = seekdb_engine_close(g_engine);
        g_engine = NULL;
        free(g_engine_dir);
        g_engine_dir = NULL;
        if (engine_result != 0)
            result = SEEKDB_INTERNAL_ERROR;
    }
    unlock_engine();
    return result;
}

int seekdb_set_binary_path(const char *path)
{
    /* Retained for API compatibility; this mode has no executable. */
    return path && path[0] ? SEEKDB_SUCCESS : SEEKDB_INVALID_ARGUMENT;
}

int seekdb_backend_binary_path(char *buf, size_t size, const char **out_path)
{
    /* The engine API takes options only, with no program name. */
    *out_path = NULL;
    return SEEKDB_SUCCESS;
}

int seekdb_backend_check_endpoint(bool found, const char *db_dir)
{
    if (!g_engine && found) {
        seekdb_set_open_error("Database directory is already served by another process: %s",
                              db_dir);
        return SEEKDB_INTERNAL_ERROR;
    }
    return SEEKDB_SUCCESS;
}

int seekdb_backend_start(SeekdbHandleImpl *h, const char *bin_path, char **argv, void **startup)
{
    if (g_engine) {
        seekdb_set_open_error("Active in-process engine endpoint is unavailable: %s", h->db_dir);
        return SEEKDB_INTERNAL_ERROR;
    }
    int argc = 0;
    while (argv[argc])
        ++argc;
    int engine_error = 0;
    const char *engine_message = NULL;
    g_engine = seekdb_engine_open(argc, (const char *const *)argv, &engine_error, &engine_message);
    if (!g_engine) {
        seekdb_set_open_error("Cannot open in-process SeekDB engine: code=%d, %s", engine_error,
                              engine_message ? engine_message : "unknown error");
        return SEEKDB_INTERNAL_ERROR;
    }
    return SEEKDB_SUCCESS;
}

int seekdb_backend_poll(void **startup) { return SEEKDB_SUCCESS; }

void seekdb_backend_finish(void *startup) {}
