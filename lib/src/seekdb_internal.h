#pragma once

#include "seekdb.h"
#include "port.h"

#include <mysql.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    char *db_dir;
    char *sock_path;         /* effective POSIX UDS path; NULL for TCP/Windows */
    char *clients_lock_path; /* <db_dir>/run/seekdb.clients */
    char *startup_lock_path; /* <db_dir>/run/seekdb.startup */
    Flock *clients_lock;     /* SH-locked for the lifetime of the handle */
    char host[64];           /* verified TCP host; empty until local endpoint discovery succeeds */
    int port;                /* verified auto-assigned TCP port; 0 until discovery succeeds */
    char server_uuid[128];   /* identity discovered locally and verified over TCP */
    /* Mirrors of Process — populated after spawn_process succeeds, so the
     * handle remembers which daemon it brought up (or was given by a
     * previous owner). 0/NULL when the handle took the fast path. */
    int64_t spawned_pid;
#ifdef _WIN32
    void *spawned_handle;
    char pipe_file_path[256]; /* <db_dir>/run/sql.pipe — server writes the pipe name here */
    char pipe_name[256]; /* contents of sql.pipe (suffix only); libmariadb prepends \\.\pipe\ */
    char pipe_path[512]; /* full \\.\pipe\... path retained for discovery diagnostics */
#else
    char *socket_alias_dir; /* /tmp/pylibseekdb-uds-<pid>-XXXXXX */
#endif
} SeekdbHandleImpl;

typedef struct {
    MYSQL *mysql;
} SeekdbConnectionImpl;

typedef struct {
    SeekdbTypeId type;
    union {
        int64_t i64;
        uint64_t u64;
        double f64;
        struct {
            char *data;
            size_t len;
        } str;
    } v;
} SeekdbValueImpl;

typedef struct {
    int column_count;
    MYSQL *mysql; /* connection that produced this result;
                    used by seekdb_result_next to call
                    mysql_errno when fetch returns NULL */
    MYSQL_RES *mysql_res;
    MYSQL_ROW current_row;          /* set by seekdb_result_next */
    unsigned long *current_lengths; /* pointer into MYSQL_RES storage,
                                      overwritten on next fetch */
} SeekdbResultImpl;
