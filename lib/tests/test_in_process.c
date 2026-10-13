#include "seekdb/seekdb.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *database;
static SeekdbHandle second;

static void *open_second(void *unused)
{
    (void)unused;
    assert(seekdb_open(database, NULL, &second) == SEEKDB_SUCCESS);
    return NULL;
}

static void query_one(SeekdbHandle handle)
{
    SeekdbConnection connection = NULL;
    SeekdbResult result = NULL;
    int64_t value = 0;
    assert(seekdb_connect(handle, NULL, true, &connection) == SEEKDB_SUCCESS);
    assert(seekdb_query(connection, "SELECT 42", 9, &result) == SEEKDB_SUCCESS);
    assert(seekdb_result_next(result) == SEEKDB_SUCCESS);
    assert(seekdb_result_get_int64(result, 0, &value) == SEEKDB_SUCCESS);
    assert(value == 42);
    assert(seekdb_result_free(result) == SEEKDB_SUCCESS);
    assert(seekdb_disconnect(connection) == SEEKDB_SUCCESS);
}

int main(void)
{
    char directory[] = "/tmp/seekdb-in-process-XXXXXX";
    char other[] = "/tmp/seekdb-in-process-other-XXXXXX";
    char original[4096], before[4096], after[4096], absolute_database[4096];
    struct sigaction sig_before, sig_after;
    assert(getcwd(original, sizeof(original)) != NULL);
    assert(sigaction(SIGTERM, NULL, &sig_before) == 0);
    assert(mkdtemp(directory) && mkdtemp(other));
    assert(chdir(directory) == 0);
    assert(getcwd(before, sizeof(before)) != NULL);
    assert(snprintf(absolute_database, sizeof(absolute_database), "%s/data", before) > 0);
    assert(access(absolute_database, F_OK) != 0 && errno == ENOENT);
    database = absolute_database;
    SeekdbHandle first = NULL, rejected = NULL;
    /* A bogus executable must be irrelevant to this build. */
    assert(seekdb_set_binary_path("/does/not/exist") == SEEKDB_SUCCESS);
    const char *bad[] = {"port", "invalid", NULL};
    assert(seekdb_open("./invalid-options", bad, &rejected) != SEEKDB_SUCCESS);
    assert(rejected == NULL && strstr(seekdb_last_open_error(), "code="));
    const char *parameters[] = {"cpu_count", "2", NULL};
    int rc = seekdb_open("./data", parameters, &first);
    if (rc != SEEKDB_SUCCESS)
        fprintf(stderr, "open failed: %s\n", seekdb_last_open_error());
    assert(rc == SEEKDB_SUCCESS && first);
    assert(seekdb_last_open_error()[0] == '\0');
    assert(sigaction(SIGTERM, NULL, &sig_after) == 0);
    assert(sig_before.sa_handler == sig_after.sa_handler);
    int status;
    errno = 0;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    /* The engine changed cwd to the now-created absolute database directory.
     * Both an absolute path and "." must share the existing engine. */
    assert(getcwd(after, sizeof(after)) && strcmp(after, absolute_database) == 0);
    SeekdbHandle same = NULL;
    assert(seekdb_open(".", NULL, &same) == SEEKDB_SUCCESS);
    assert(seekdb_close(same) == SEEKDB_SUCCESS);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, open_second, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(seekdb_open(other, NULL, &rejected) != SEEKDB_SUCCESS && !rejected);
    query_one(first);
    assert(seekdb_close(first) == SEEKDB_SUCCESS);
    query_one(second);
    assert(seekdb_close(second) == SEEKDB_SUCCESS);
    assert(getcwd(after, sizeof(after)) && strcmp(before, after) == 0);
    assert(seekdb_open("./data", NULL, &rejected) != SEEKDB_SUCCESS && !rejected);
    assert(strstr(seekdb_last_open_error(), "code="));
    assert(chdir(original) == 0);
    printf("Data retained for inspection: %s\n", directory);
    puts("IN_PROCESS_COMPLETED");
    return 0;
}
