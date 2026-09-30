/*
 * Small Win32 helpers taking UTF-8 paths.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* malloc'd conversions; NULL on failure */
wchar_t *utf8_to_wide(const char *s);
char *wide_to_utf8(const wchar_t *s);

bool fs_exists(const char *path);
/* Creates the directory and any missing parents. */
bool fs_mkdirs(const char *path);
/* Returns false with *error set to the Win32 error code. */
bool fs_delete(const char *path, unsigned long *error);

/* Monotonic clock (QueryPerformanceCounter) in nanoseconds; the same clock
 * libobs' os_gettime_ns() reads. */
int64_t qpc_now_ns(void);

/* Human-readable text for a Win32 error code. */
void win_error_text(unsigned long code, char *buf, size_t size);
