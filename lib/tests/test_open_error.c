#include "seekdb.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static void *check_thread(void *unused)
{
    (void)unused;
    assert(seekdb_last_open_error()[0] == '\0');
    const char *parameters[] = {"mysql_port", "1234", NULL};
    SeekdbHandle handle = (SeekdbHandle)1;
    assert(seekdb_open("unused", parameters, &handle) == SEEKDB_INVALID_ARGUMENT);
    assert(handle == NULL);
    assert(strstr(seekdb_last_open_error(), "Invalid parameters"));
    return NULL;
}

int main(int argc, char **argv)
{
    SeekdbHandle handle = (SeekdbHandle)1;
    assert(seekdb_last_open_error()[0] == '\0');
    assert(seekdb_open(NULL, NULL, &handle) == SEEKDB_INVALID_ARGUMENT);
    assert(handle == NULL);
    char saved[2048];
    snprintf(saved, sizeof(saved), "%s", seekdb_last_open_error());
    assert(strstr(saved, "db_dir"));
    pthread_t thread;
    assert(pthread_create(&thread, NULL, check_thread, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(strcmp(saved, seekdb_last_open_error()) == 0);
    puts("OPEN_ERROR_TLS_OK invalid arguments, NULL output, thread isolation");
    if (argc == 3) {
        assert(seekdb_set_binary_path(argv[1]) == SEEKDB_SUCCESS);
        assert(seekdb_open(argv[2], NULL, &handle) == SEEKDB_SUCCESS);
        assert(seekdb_last_open_error()[0] == '\0');
        assert(seekdb_close(handle) == SEEKDB_SUCCESS);
        puts("OPEN_ERROR_CLEAR_OK successful open clears prior error");
    }
    return 0;
}
