/*
 * Logging for the OBS-free parts of the plugin. The plugin routes messages to
 * blog() with a "[gold-clips]" prefix; tests and tools print them to stderr.
 */
#pragma once

enum {
	GC_LOG_ERROR = 100,
	GC_LOG_WARNING = 200,
	GC_LOG_INFO = 300,
	GC_LOG_DEBUG = 400,
};

typedef void (*gc_log_sink_fn)(int level, const char *msg);

/* NULL restores the default stderr sink. Set once, before any thread starts. */
void gc_log_set_sink(gc_log_sink_fn sink);

void gc_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
