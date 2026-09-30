#include "procrun.h"

#include "strbuf.h"
#include "winutil.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

static void append_program(struct strbuf *sb, const char *arg)
{
	/* argv[0] is parsed differently: up to the next quote, no escapes. */
	if (*arg && !strpbrk(arg, " \t")) {
		sb_append(sb, arg);
		return;
	}
	sb_append_ch(sb, '"');
	sb_append(sb, arg);
	sb_append_ch(sb, '"');
}

static void append_arg(struct strbuf *sb, const char *arg)
{
	if (*arg && !strpbrk(arg, " \t\n\v\"")) {
		sb_append(sb, arg);
		return;
	}

	sb_append_ch(sb, '"');
	for (const char *p = arg;; p++) {
		size_t backslashes = 0;
		while (*p == '\\') {
			backslashes++;
			p++;
		}
		if (!*p) {
			/* double them so the closing quote stays a quote */
			for (size_t i = 0; i < backslashes * 2; i++)
				sb_append_ch(sb, '\\');
			break;
		}
		if (*p == '"') {
			for (size_t i = 0; i < backslashes * 2 + 1; i++)
				sb_append_ch(sb, '\\');
		} else {
			for (size_t i = 0; i < backslashes; i++)
				sb_append_ch(sb, '\\');
		}
		sb_append_ch(sb, *p);
	}
	sb_append_ch(sb, '"');
}

char *cmdline_build(const char *const *argv, size_t argc)
{
	struct strbuf sb;

	sb_init(&sb);
	for (size_t i = 0; i < argc; i++) {
		if (i == 0) {
			append_program(&sb, argv[i]);
		} else {
			sb_append_ch(&sb, ' ');
			append_arg(&sb, argv[i]);
		}
	}
	if (sb.failed || !sb.s) {
		sb_free(&sb);
		return NULL;
	}
	return sb.s;
}

static void drain_pipe(HANDLE pipe, struct proc_result *res, size_t *len)
{
	char buf[1024];
	DWORD avail, got;

	while (PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
		if (!ReadFile(pipe, buf, avail < sizeof(buf) ? avail : (DWORD)sizeof(buf), &got, NULL) || !got)
			break;
		if (*len + 1 < sizeof(res->output)) {
			size_t n = got;
			if (*len + n + 1 > sizeof(res->output))
				n = sizeof(res->output) - 1 - *len;
			memcpy(res->output + *len, buf, n);
			*len += n;
			res->output[*len] = '\0';
		}
	}
}

void proc_run(const char *const *argv, size_t argc, unsigned long timeout_ms, void *cancel_event,
	      struct proc_result *res)
{
	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE read_end = NULL, write_end = NULL, nul_in = INVALID_HANDLE_VALUE;
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
	SIZE_T attrs_size = 0;
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	HANDLE inherit[2];
	char *cmd = NULL;
	wchar_t *wcmd = NULL;
	size_t out_len = 0;
	ULONGLONG deadline;

	memset(res, 0, sizeof(*res));
	res->status = PROC_START_FAILED;

	cmd = cmdline_build(argv, argc);
	wcmd = cmd ? utf8_to_wide(cmd) : NULL;
	if (!wcmd) {
		res->start_error = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}

	if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
		res->start_error = GetLastError();
		goto done;
	}
	SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
	nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
	if (nul_in == INVALID_HANDLE_VALUE) {
		res->start_error = GetLastError();
		goto done;
	}

	/* Hand the child only these two handles, not every inheritable handle
	 * the host process happens to have open. */
	InitializeProcThreadAttributeList(NULL, 1, 0, &attrs_size);
	attrs = malloc(attrs_size);
	if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attrs_size)) {
		res->start_error = GetLastError();
		free(attrs);
		attrs = NULL;
		goto done;
	}
	inherit[0] = write_end;
	inherit[1] = nul_in;
	if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), NULL,
				       NULL)) {
		res->start_error = GetLastError();
		goto done;
	}

	memset(&si, 0, sizeof(si));
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = nul_in;
	si.StartupInfo.hStdOutput = write_end;
	si.StartupInfo.hStdError = write_end;
	si.lpAttributeList = attrs;

	if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
			    &si.StartupInfo, &pi)) {
		res->start_error = GetLastError();
		goto done;
	}
	CloseHandle(pi.hThread);
	CloseHandle(write_end);
	write_end = NULL;

	deadline = GetTickCount64() + timeout_ms;
	for (;;) {
		HANDLE waits[2] = {pi.hProcess, (HANDLE)cancel_event};
		DWORD w = WaitForMultipleObjects(cancel_event ? 2 : 1, waits, FALSE, 50);

		drain_pipe(read_end, res, &out_len);
		if (w == WAIT_OBJECT_0) {
			DWORD code = 0;
			GetExitCodeProcess(pi.hProcess, &code);
			res->exit_code = code;
			res->status = PROC_EXITED;
			break;
		}
		if (w == WAIT_OBJECT_0 + 1 || GetTickCount64() >= deadline || w == WAIT_FAILED) {
			/* our own child: stop it */
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 5000);
			drain_pipe(read_end, res, &out_len);
			res->status = w == WAIT_OBJECT_0 + 1 ? PROC_CANCELLED : PROC_TIMEOUT;
			break;
		}
	}
	CloseHandle(pi.hProcess);

done:
	if (attrs) {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
	}
	if (nul_in != INVALID_HANDLE_VALUE)
		CloseHandle(nul_in);
	if (write_end)
		CloseHandle(write_end);
	if (read_end)
		CloseHandle(read_end);
	free(wcmd);
	free(cmd);
}
