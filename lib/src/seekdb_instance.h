#pragma once

#include "seekdb_internal.h"
#include <stdbool.h>

/* Shared directory preparation, arguments, discovery, and diagnostics. */
int seekdb_open_instance(const char *db_dir, const char **parameters, SeekdbHandle *out_handle);
void seekdb_destroy_handle(SeekdbHandleImpl *handle);
int seekdb_validate_parameters(const char *const *parameters);
void seekdb_set_open_error(const char *fmt, ...);

/* Implemented by exactly one backend selected by CMake. The in-process
 * backend holds its engine mutex throughout seekdb_open_instance(). */
int seekdb_backend_binary_path(char *buf, size_t size, const char **out_path);
int seekdb_backend_check_endpoint(bool found, const char *db_dir);
int seekdb_backend_start(SeekdbHandleImpl *handle, const char *bin_path, char **argv,
                         void **startup);
/* poll releases and clears startup if the child exits; finish transfers any
 * remaining startup state to the reaper. In-process startup needs no state. */
int seekdb_backend_poll(void **startup);
void seekdb_backend_finish(void *startup);
