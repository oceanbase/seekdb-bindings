#include "seekdb/seekdb.h"
#include "seekdb_internal.h"
#include "tlog.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *seekdb_malloc(size_t size) { return malloc(size); }
void seekdb_free(void *ptr) { free(ptr); }

int seekdb_connection_options(SeekdbHandle handle, SeekdbConnectionOptions *out_options)
{
    if (!handle || !out_options)
        return SEEKDB_INVALID_ARGUMENT;

    SeekdbHandleImpl *h = (SeekdbHandleImpl *)handle;
    memset(out_options, 0, sizeof(*out_options));
    out_options->user = "root";

#ifdef _WIN32
    if (h->port != 0) {
        if (h->host[0] == '\0' || h->port < 0 || h->port > 65535 || h->server_uuid[0] == '\0')
            return SEEKDB_INTERNAL_ERROR;
        out_options->transport = SEEKDB_CONNECTION_TRANSPORT_TCP;
        out_options->host = h->host;
        out_options->port = (unsigned int)h->port;
    }
    else {
        if (h->pipe_path[0] == '\0')
            return SEEKDB_INTERNAL_ERROR;
        out_options->transport = SEEKDB_CONNECTION_TRANSPORT_NAMED_PIPE;
        out_options->named_pipe = h->pipe_path;
    }
#else
    if (!h->sock_path)
        return SEEKDB_INTERNAL_ERROR;
    out_options->transport = SEEKDB_CONNECTION_TRANSPORT_UNIX_SOCKET;
    out_options->unix_socket = h->sock_path;
#endif

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
#ifdef _WIN32
    const bool use_tcp = h->port != 0;
    if (use_tcp &&
        (h->host[0] == '\0' || h->port < 0 || h->port > 65535 || h->server_uuid[0] == '\0'))
        return SEEKDB_INTERNAL_ERROR;
    if (!use_tcp && h->pipe_name[0] == '\0')
        return SEEKDB_INTERNAL_ERROR;
    if (use_tcp) {
        tlog("seekdb_connect: tcp=%s:%d db=%s autocommit=%d\n", h->host, h->port,
             database ? database : "(null)", (int)autocommit);
    }
    else {
        tlog("seekdb_connect: pipe=\\\\.\\pipe\\%s db=%s autocommit=%d\n", h->pipe_name,
             database ? database : "(null)", (int)autocommit);
    }
#else
    const bool use_tcp = false;
    if (!h->sock_path)
        return SEEKDB_INTERNAL_ERROR;
    tlog("seekdb_connect: sock=%s db=%s autocommit=%d\n", h->sock_path,
         database ? database : "(null)", (int)autocommit);
#endif

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
#ifdef _WIN32
    if (!use_tcp)
        mysql_options(c->mysql, MYSQL_OPT_NAMED_PIPE, NULL);
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
        free(v->v.str.data);
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
