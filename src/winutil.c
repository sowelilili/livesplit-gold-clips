#include "winutil.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

wchar_t *utf8_to_wide(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w;

	if (n <= 0)
		return NULL;
	w = malloc((size_t)n * sizeof(wchar_t));
	if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
		free(w);
		w = NULL;
	}
	return w;
}

char *wide_to_utf8(const wchar_t *s)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
	char *u;

	if (n <= 0)
		return NULL;
	u = malloc((size_t)n);
	if (u && WideCharToMultiByte(CP_UTF8, 0, s, -1, u, n, NULL, NULL) <= 0) {
		free(u);
		u = NULL;
	}
	return u;
}

bool fs_exists(const char *path)
{
	wchar_t *w = utf8_to_wide(path);
	DWORD attr;

	if (!w)
		return false;
	attr = GetFileAttributesW(w);
	free(w);
	return attr != INVALID_FILE_ATTRIBUTES;
}

bool fs_mkdirs(const char *path)
{
	wchar_t *w = utf8_to_wide(path);
	size_t len;
	DWORD attr;

	if (!w)
		return false;
	len = wcslen(w);
	for (size_t i = 0; i < len; i++)
		if (w[i] == L'/')
			w[i] = L'\\';

	/* Try every prefix that ends at a separator; failures on the drive or
	 * existing parents are expected and ignored. */
	for (size_t i = 1; i < len; i++) {
		if (w[i] != L'\\' || w[i - 1] == L'\\' || w[i - 1] == L':')
			continue;
		w[i] = L'\0';
		CreateDirectoryW(w, NULL);
		w[i] = L'\\';
	}
	CreateDirectoryW(w, NULL);

	attr = GetFileAttributesW(w);
	free(w);
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool fs_delete(const char *path, unsigned long *error)
{
	wchar_t *w = utf8_to_wide(path);
	BOOL ok;

	if (!w) {
		if (error)
			*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	ok = DeleteFileW(w);
	if (!ok && error)
		*error = GetLastError();
	free(w);
	return ok != 0;
}

int64_t qpc_now_ns(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER now;

	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	/* split to avoid overflowing 64 bits, as libobs' util_mul_div64 does */
	return (int64_t)((now.QuadPart / freq.QuadPart) * 1000000000LL +
			 (now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
}

void win_error_text(unsigned long code, char *buf, size_t size)
{
	wchar_t wbuf[512];
	char *u;
	DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, wbuf,
				 (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])), NULL);

	while (n > 0 && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n' || wbuf[n - 1] == L' ' || wbuf[n - 1] == L'.'))
		wbuf[--n] = L'\0';
	u = n ? wide_to_utf8(wbuf) : NULL;
	if (u) {
		snprintf(buf, size, "%s (error %lu)", u, code);
		free(u);
	} else {
		snprintf(buf, size, "error %lu", code);
	}
}
