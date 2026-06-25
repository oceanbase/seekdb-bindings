#pragma once

#include <stdio.h>

/* Timestamped debug print. Format: [HH:MM:SS.mmm T=<tid>] ...
 * Compile-gated via SEEKDB_DRIVER_ENABLE_LOG — when not defined, tlog(...) expands
 * to a sizeof(printf(...)) probe: arguments are not evaluated (sizeof never
 * evaluates its operand for non-VLA expressions) but the compiler still sees
 * them as "used", suppressing log-only unused-variable warnings.
 *
 * Enable from the top-level build with:
 *     cmake -B build -DSEEKDB_DRIVER_ENABLE_LOG=ON
 */
#ifdef SEEKDB_DRIVER_ENABLE_LOG

#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <sys/timeb.h>
#else
#include <pthread.h>
#include <sys/time.h>
#endif

static inline void tlog(const char *fmt, ...)
{
    int ms;
    time_t tt;
    struct tm tm_buf;
    unsigned long long tid;

#ifdef _WIN32
    struct __timeb64 tb;
    _ftime64_s(&tb);
    tt = (time_t)tb.time;
    ms = (int)tb.millitm;
    localtime_s(&tm_buf, &tt);
    tid = (unsigned long long)GetCurrentThreadId();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    tt = (time_t)tv.tv_sec;
    ms = (int)(tv.tv_usec / 1000);
    localtime_r(&tt, &tm_buf);
    tid = (unsigned long long)(uintptr_t)pthread_self();
#endif

    char prefix[64];
    int p = snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d.%03d T=%llx] ", tm_buf.tm_hour,
                     tm_buf.tm_min, tm_buf.tm_sec, ms, tid);

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char buf[600];
    snprintf(buf, sizeof(buf), "%.*s%s", p, prefix, msg);
    fputs(buf, stdout);
}

#else /* SEEKDB_DRIVER_ENABLE_LOG not defined */

#define tlog(...) ((void)sizeof(printf(__VA_ARGS__)))

#endif
