/*
 * seekdb CLI — interactive SQL client, similar to the mariadb/mysql client.
 *
 * Usage: seekdb_cli [db_dir]
 *
 * If db_dir is omitted, defaults to "./seekdb.db" in the current
 * working directory. The seekdb server binary is auto-discovered next
 * to the libseekdb shared library.
 */

#include "seekdb_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strncasecmp _strnicmp
#define strdup _strdup
#endif

/* ---------------------------------------------------------------- types -- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}
static void buf_clear(Buf *b) { b->len = 0; }
static void buf_free(Buf *b)
{
    free(b->data);
    buf_init(b);
}

static void buf_append(Buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* ------------------------------------------------------- result display -- */

static void print_separator(int ncols, int *widths)
{
    for (int i = 0; i < ncols; i++) {
        putchar('+');
        for (int j = 0; j < widths[i] + 2; j++)
            putchar('-');
    }
    puts("+");
}

static void print_row(int ncols, int *widths, const char **cells)
{
    for (int i = 0; i < ncols; i++) {
        const char *val = cells[i] ? cells[i] : "NULL";
        printf("| %-*s ", widths[i], val);
    }
    puts("|");
}

static void print_result(SeekdbResult result)
{
    SeekdbResultImpl *r = result;
    if (!r->mysql_res)
        return;

    int ncols = r->column_count;
    int *widths = calloc((size_t)ncols, sizeof(int));
    const char **names = calloc((size_t)ncols, sizeof(char *));

    /* Column names and initial widths. */
    for (int i = 0; i < ncols; i++) {
        seekdb_result_column_name(result, i, &names[i]);
        widths[i] = (int)strlen(names[i]);
    }

    /* Buffer all rows to compute column widths. */
    int row_cap = 64, nrows = 0;
    char ***rows = malloc((size_t)row_cap * sizeof(char **));

    while (seekdb_result_next(result) == SEEKDB_SUCCESS) {
        if (nrows == row_cap) {
            row_cap *= 2;
            rows = realloc(rows, (size_t)row_cap * sizeof(char **));
        }
        char **row = malloc((size_t)ncols * sizeof(char *));
        for (int i = 0; i < ncols; i++) {
            const char *data = r->current_row[i];
            if (!data) {
                row[i] = strdup("NULL");
                if (4 > widths[i])
                    widths[i] = 4;
            }
            else {
                size_t len = r->current_lengths[i];
                row[i] = malloc(len + 1);
                memcpy(row[i], data, len);
                row[i][len] = '\0';
                if ((int)len > widths[i])
                    widths[i] = (int)len;
            }
        }
        rows[nrows++] = row;
    }

    /* Print table. */
    print_separator(ncols, widths);
    print_row(ncols, widths, names);
    print_separator(ncols, widths);
    for (int i = 0; i < nrows; i++)
        print_row(ncols, widths, (const char **)rows[i]);
    print_separator(ncols, widths);

    printf("%d %s in set\n\n", nrows, nrows == 1 ? "row" : "rows");

    /* Cleanup. */
    for (int i = 0; i < nrows; i++) {
        for (int j = 0; j < ncols; j++)
            free(rows[i][j]);
        free(rows[i]);
    }
    free(rows);
    free(widths);
    free(names);
}

/* ------------------------------------------------- query execution ----- */

static void execute(SeekdbConnection conn, const char *sql, size_t len)
{
    SeekdbResult result = NULL;
    int rc = seekdb_query(conn, sql, (int64_t)len, &result);
    if (rc != SEEKDB_SUCCESS) {
        fprintf(stderr, "ERROR: query failed (rc=%d)\n", rc);
        return;
    }

    SeekdbResultImpl *r = result;
    if (r->mysql_res) {
        print_result(result);
    }
    else {
        puts("Query OK");
    }

    seekdb_result_free(result);
}

/* ---------------------------------------------------------- line input -- */

/* Read one line of input from stdin into a fresh malloc'd buffer.
 * Trailing '\n' is stripped. Returns NULL on EOF. Lines longer than
 * 4 KiB are truncated. */
static char *read_line(const char *prompt)
{
    fputs(prompt, stdout);
    fflush(stdout);

    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin))
        return NULL;

    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n')
        buf[--n] = '\0';

    char *r = (char *)malloc(n + 1);
    if (!r)
        return NULL;
    memcpy(r, buf, n + 1);
    return r;
}

/* ----------------------------------------- check for quit/exit command -- */

static int is_quit(const char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    return strncasecmp(s, "quit", 4) == 0 || strncasecmp(s, "exit", 4) == 0;
}

/* ---------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    const char *db_dir = (argc >= 2) ? argv[1] : "./seekdb.db";

    SeekdbHandle handle = NULL;
    if (seekdb_open(db_dir, NULL, &handle) != SEEKDB_SUCCESS) {
        fprintf(stderr, "seekdb_open failed\n");
        return 1;
    }

    SeekdbConnection conn = NULL;
    if (seekdb_connect(handle, NULL, true, &conn) != SEEKDB_SUCCESS) {
        fprintf(stderr, "seekdb_connect failed\n");
        seekdb_close(handle);
        return 1;
    }

    printf("Welcome to the SeekDB monitor.\n"
           "Type 'help' for help. Terminate each statement with ';'.\n\n");

    Buf query;
    buf_init(&query);

    for (;;) {
        const char *prompt = query.len == 0 ? "seekdb> " : "     -> ";
        char *line = read_line(prompt);
        if (!line)
            break; /* EOF / Ctrl-D */

        /* Skip empty lines. */
        const char *p = line;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0') {
            free(line);
            continue;
        }

        if (query.len == 0 && is_quit(line)) {
            free(line);
            break;
        }

        /* Append to query buffer. */
        if (query.len)
            buf_append(&query, " ", 1);
        buf_append(&query, line, strlen(line));
        free(line);

        /* Check for ';' terminator. */
        size_t end = query.len;
        while (end > 0 && isspace((unsigned char)query.data[end - 1]))
            end--;
        if (end > 0 && query.data[end - 1] == ';') {
            end--; /* strip the ';' */
            execute(conn, query.data, end);
            buf_clear(&query);
        }
    }

    buf_free(&query);
    printf("Bye\n");
    seekdb_disconnect(conn);
    seekdb_close(handle);
    return 0;
}
