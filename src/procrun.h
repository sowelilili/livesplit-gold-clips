/*
 * Running a console program (ffmpeg) without a window: UTF-8 arguments are
 * quoted into a Windows command line, stdout and stderr are captured, and the
 * run is bounded by a timeout and an optional cancel event.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Builds the command line for argv as a malloc'd UTF-8 string. The
 * quoting round-trips through CommandLineToArgvW and the MSVC CRT. */
char *cmdline_build(const char *const *argv, size_t argc);

enum proc_status {
	PROC_EXITED,
	PROC_START_FAILED,
	PROC_TIMEOUT,
	PROC_CANCELLED,
};

#define PROC_OUTPUT_MAX 4096

struct proc_result {
	enum proc_status status;
	unsigned long exit_code;  /* valid for PROC_EXITED */
	unsigned long start_error; /* Win32 error for PROC_START_FAILED */
	char output[PROC_OUTPUT_MAX]; /* first bytes of stdout+stderr */
};

/* cancel_event may be NULL. */
void proc_run(const char *const *argv, size_t argc, unsigned long timeout_ms, void *cancel_event,
	      struct proc_result *res);
