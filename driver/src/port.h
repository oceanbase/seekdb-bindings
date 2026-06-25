#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OK              =  0,
    ERR             = -1,
    ERR_INVALID_ARG = -2
};

typedef struct Flock Flock;

typedef struct Process {
    int64_t pid;
#ifdef _WIN32
    void *handle;
#endif
} Process;

typedef enum {
    FLOCK_SHARED,
    FLOCK_EXCLUSIVE
} FlockMode;

int flock_open(const char *path, Flock **out_flock);
int flock_acquire(Flock *lock, FlockMode mode);
int flock_try_acquire(Flock *lock, FlockMode mode);
void flock_release(Flock *lock);
int flock_close(Flock *lock);

int spawn_process(const char *bin_path, char *const argv[], Process **out_proc);
int reap_process(Process *proc);
int is_server_reaped(int64_t pid);
int terminate_process(int64_t pid, int graceful);

int get_module_dir(char *buf, size_t buflen);

int ensure_dir(const char *path);

/* Returns 1 if `path` is a directory containing at least one entry other than
 * "." / "..", 0 otherwise (missing, empty, or not a directory). */
int dir_has_entries(const char *path);

#ifdef __cplusplus
}
#endif
