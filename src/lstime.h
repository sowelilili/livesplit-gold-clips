/*
 * LiveSplit time values. The server prints times with C#'s TimeSpan constant
 * format, [-][d.]hh:mm:ss[.fffffff]; shorter forms (mm:ss.ff, ss.ff) are
 * accepted too. Times are kept as TimeSpan ticks (100 ns).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LSTIME_TICKS_PER_SECOND 10000000LL

/* Returns false for "no time": NULL, empty, "-" or anything unparseable. */
bool lstime_parse(const char *text, int64_t *ticks);

/* Windows-safe file name form, hundredths truncated: 23.45s, 1m23.45s, 1h02m03.45s */
void lstime_format_filename(int64_t ticks, char *buf, size_t size);

/* LiveSplit-style display form for logs: 23.45, 1:23.45, 1:02:03.45 */
void lstime_format_display(int64_t ticks, char *buf, size_t size);
