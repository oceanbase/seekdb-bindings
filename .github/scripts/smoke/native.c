#include <seekdb/seekdb.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    SeekdbHandle db = NULL;
    SeekdbConnection connection = NULL;
    SeekdbResult result = NULL;
    int status = 1;
    int64_t value = 0;
    const char *sql = "SELECT 42";
    if (argc != 2 || seekdb_open(argv[1], NULL, &db) != SEEKDB_SUCCESS) {
        fprintf(stderr, "seekdb_open: %s\n", seekdb_last_open_error());
        goto cleanup;
    }
    if (seekdb_connect(db, "test", true, &connection) != SEEKDB_SUCCESS ||
        seekdb_query(connection, sql, (int64_t)strlen(sql), &result) != SEEKDB_SUCCESS ||
        seekdb_result_next(result) != SEEKDB_SUCCESS ||
        seekdb_result_get_int64(result, 0, &value) != SEEKDB_SUCCESS || value != 42) {
        fprintf(stderr, "seekdb SQL smoke failed\n");
        goto cleanup;
    }
    status = 0;
cleanup:
    if (result != NULL) seekdb_result_free(result);
    if (connection != NULL) seekdb_disconnect(connection);
    if (db != NULL && seekdb_close(db) != SEEKDB_SUCCESS) status = 1;
    if (status == 0) puts("seekdb C SDK smoke passed: SELECT 42");
    return status;
}
