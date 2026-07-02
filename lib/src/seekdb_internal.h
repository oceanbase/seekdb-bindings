#pragma once

#include "seekdb.h"
#include "port.h"

#include <mysql.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    char *db_dir;
    char sock_path[256];
    char clients_lock_path[256];
    char startup_lock_path[256];
    Flock *clients_lock; /* SH-locked for the lifetime of the handle */
    char host[64];       /* set to "127.0.0.1" when caller passes a non-zero port */
    int port; /* 0 ⇒ local transport (UDS on POSIX, named pipe on Windows); non-zero ⇒ TCP host:port
               */
    /* Mirrors of Process — populated after spawn_process succeeds, so the
     * handle remembers which daemon it brought up (or was given by a
     * previous owner). 0/NULL when the handle took the fast path. */
    int64_t spawned_pid;
#ifdef _WIN32
    void *spawned_handle;
    char pipe_file_path[256]; /* <db_dir>/run/sql.pipe — server writes the pipe name here */
    char pipe_name[256]; /* contents of sql.pipe (suffix only); libmariadb prepends \\.\pipe\ */
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
