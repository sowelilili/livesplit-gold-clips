#include "gclog.h"

#include <stdarg.h>
#include <stdio.h>

static void default_sink(int level, const char *msg)
{
	const char *tag = level <= GC_LOG_ERROR     ? "error"
			  : level <= GC_LOG_WARNING ? "warning"
			  : level <= GC_LOG_INFO    ? "info"
						    : "debug";
	fprintf(stderr, "[gold-clips] %s: %s\n", tag, msg);
	fflush(stderr);
}

static gc_log_sink_fn log_sink = default_sink;

void gc_log_set_sink(gc_log_sink_fn sink)
{
	log_sink = sink ? sink : default_sink;
}

void gc_log(int level, const char *fmt, ...)
{
	char buf[2048];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	log_sink(level, buf);
}
