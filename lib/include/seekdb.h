#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *SeekdbHandle;
typedef void *SeekdbConnection;
typedef void *SeekdbResult;
typedef void *SeekdbType;
typedef void *SeekdbValue;

typedef enum {
    SEEKDB_SUCCESS = 0,
    SEEKDB_INTERNAL_ERROR = -1,
    SEEKDB_INVALID_ARGUMENT = -2,
    SEEKDB_NO_MORE_ROWS = -3,
} SeekdbReturnCode;

typedef enum {
    SEEKDB_TYPE_NULL,
    SEEKDB_TYPE_INT64,
    SEEKDB_TYPE_UINT64,
    SEEKDB_TYPE_FLOAT,
    SEEKDB_TYPE_DECIMAL,
    SEEKDB_TYPE_DATE,
    SEEKDB_TYPE_DATETIME,
    SEEKDB_TYPE_TIMESTAMP,
    SEEKDB_TYPE_VARCHAR,
} SeekdbTypeId;

/* Open a seekdb instance rooted at db_dir.
 *
 * parameters is an optional NULL-terminated array of key/value pairs:
 *   {"port", "3306", "memory_limit", "10G", "syslog_max_file", "1000", NULL}
 *
 * Driver-reserved keys (consumed by libseekdb, not forwarded to the server):
 *   port — TCP port for connect; omit or "0" for local transport (UDS/pipe).
 *
 * All other keys are seekdb server parameters, passed as --parameter on first
 * init only. On first init the driver always seeds memory_limit=1G and
 * log_disk_size=2G unless the caller overrides them; additional server keys
 * may also be supplied. On restart, persisted values are kept (issue #26). */
int seekdb_open(const char *db_dir, const char **parameters, SeekdbHandle *out_handle);
int seekdb_close(SeekdbHandle handle);

int seekdb_connect(SeekdbHandle handle, const char *database, bool autocommit,
                   SeekdbConnection *out_connection);
int seekdb_disconnect(SeekdbConnection connection);

int seekdb_last_error(SeekdbConnection connection, int *out_errno, const char **out_msg);

int seekdb_query(SeekdbConnection connection, const char *sql, int64_t sql_len,
                 SeekdbResult *out_result);

int seekdb_result_free(SeekdbResult result);
int seekdb_result_column_count(SeekdbResult result, int64_t *out_ncolumn);
int seekdb_result_column_name(SeekdbResult result, int64_t index, const char **out_name);
int seekdb_result_column_type_id(SeekdbResult result, int64_t index, SeekdbTypeId *out_typeid);
int seekdb_result_row_count(SeekdbResult result, int64_t *out_nrows);
int seekdb_result_next(SeekdbResult result);
int seekdb_result_get_int64(SeekdbResult result, int64_t index, int64_t *out_value);
int seekdb_result_get_uint64(SeekdbResult result, int64_t index, uint64_t *out_value);
int seekdb_result_get_float(SeekdbResult result, int64_t index, double *out_value);
int seekdb_result_get_str(SeekdbResult result, int64_t index, const char **out_data,
                          size_t *out_len, int *out_is_null);

int seekdb_trx_begin(SeekdbConnection connection);
int seekdb_trx_commit(SeekdbConnection connection);
int seekdb_trx_rollback(SeekdbConnection connection);

int seekdb_value_free(SeekdbValue value);
int seekdb_value_create_int64(int64_t int_value, SeekdbValue *out_value);
int seekdb_value_get_int64(SeekdbValue value, int64_t *out_value);

void *seekdb_malloc(size_t size);
void seekdb_free(void *ptr);

#ifdef __cplusplus
}
#endif
