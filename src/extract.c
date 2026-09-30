#include "extract.h"

#include "gclog.h"
#include "lstime.h"
#include "procrun.h"
#include "winutil.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_BUF 2048
#define DELETE_ATTEMPTS 10

void extract_job_free(struct extract_job *job)
{
	if (!job)
		return;
	free(job->replay_path);
	free(job->folder);
	free(job->ffmpeg);
	clip_free_array(job->clips, job->num_clips);
	free(job);
}

static void format_ms(char *buf, size_t size, const char *sign, int64_t ms)
{
	/* integer formatting: no locale can turn the dot into a comma */
	snprintf(buf, size, "%s%lld.%03lld", sign, (long long)(ms / 1000), (long long)(ms % 1000));
}

size_t ffmpeg_clip_args(const char *ffmpeg, const char *input, int64_t sseof_ms, int64_t dur_ms, const char *output,
			char num_buf[2][32], const char *argv[24])
{
	size_t n = 0;

	format_ms(num_buf[0], 32, "-", sseof_ms);
	format_ms(num_buf[1], 32, "", dur_ms);

	argv[n++] = ffmpeg;
	argv[n++] = "-hide_banner";
	argv[n++] = "-nostdin";
	argv[n++] = "-loglevel";
	argv[n++] = "error";
	argv[n++] = "-sseof";
	argv[n++] = num_buf[0];
	argv[n++] = "-i";
	argv[n++] = input;
	argv[n++] = "-t";
	argv[n++] = num_buf[1];
	argv[n++] = "-map"; /* keep every audio track, not just the first */
	argv[n++] = "0";
	argv[n++] = "-c";
	argv[n++] = "copy";
	argv[n++] = "-avoid_negative_ts";
	argv[n++] = "make_zero";
	argv[n++] = "-n"; /* never overwrite */
	argv[n++] = output;
	argv[n] = NULL;
	return n;
}

static bool exists_cb(void *ctx, const char *path)
{
	(void)ctx;
	return fs_exists(path);
}

static bool is_cancelled(void *cancel_event)
{
	return cancel_event && WaitForSingleObject((HANDLE)cancel_event, 0) == WAIT_OBJECT_0;
}

static void log_output(int level, const char *output)
{
	const char *p = output;

	while (*p) {
		size_t n = strcspn(p, "\r\n");
		if (n)
			gc_log(level, "  ffmpeg: %.*s", (int)n, p);
		p += n;
		while (*p == '\r' || *p == '\n')
			p++;
	}
}

static bool cut_clip(const struct extract_job *job, const struct clip *c, const char *ext, void *cancel_event)
{
	char dir[PATH_BUF], base[512], out[PATH_BUF];
	char nums[2][32];
	const char *argv[24];
	size_t argc;
	int64_t sseof_ms, dur_ms;
	bool clamped;
	struct proc_result *res;
	bool ok = false;

	if (!clip_output_dir(job->replay_path, job->folder, c->game, c->category, dir, sizeof(dir))) {
		gc_log(GC_LOG_ERROR, "clip for \"%s\": output path too long", c->split_name);
		return false;
	}
	if (!fs_mkdirs(dir)) {
		gc_log(GC_LOG_ERROR, "cannot create folder %s", dir);
		return false;
	}
	clip_file_base(c->split_name, c->seg_ticks, base, sizeof(base));
	if (!clip_unique_path(dir, base, ext, exists_cb, NULL, out, sizeof(out))) {
		gc_log(GC_LOG_ERROR, "no free file name for %s/%s%s", dir, base, ext);
		return false;
	}

	clamped = clip_cut_params(job->t_req_ns, c->start_ns, c->end_ns, job->buffer_seconds, &sseof_ms, &dur_ms);
	if (dur_ms <= 0) {
		gc_log(GC_LOG_ERROR,
		       "clip for \"%s\": the whole window is older than the saved replay (%.1f s); nothing to cut",
		       c->split_name, job->buffer_seconds);
		return false;
	}
	if (clamped)
		gc_log(GC_LOG_INFO, "clip for \"%s\" starts late: the saved replay only reaches back %.1f s",
		       c->split_name, job->buffer_seconds);

	argc = ffmpeg_clip_args(job->ffmpeg, job->replay_path, sseof_ms, dur_ms, out, nums, argv);
	gc_log(GC_LOG_DEBUG, "ffmpeg -sseof %s -t %s -> %s", nums[0], nums[1], out);

	res = malloc(sizeof(*res));
	if (!res)
		return false;
	proc_run(argv, argc, EXTRACT_TIMEOUT_MS, cancel_event, res);

	switch (res->status) {
	case PROC_START_FAILED: {
		char err[256];
		win_error_text(res->start_error, err, sizeof(err));
		gc_log(GC_LOG_ERROR,
		       "could not start ffmpeg (\"%s\"): %s. Install ffmpeg and put it on PATH, "
		       "or set ffmpeg= in the settings to its full path",
		       job->ffmpeg, err);
		break;
	}
	case PROC_TIMEOUT:
		gc_log(GC_LOG_ERROR, "ffmpeg did not finish within %d s cutting %s", EXTRACT_TIMEOUT_MS / 1000, out);
		log_output(GC_LOG_ERROR, res->output);
		break;
	case PROC_CANCELLED:
		gc_log(GC_LOG_WARNING, "cutting %s was cancelled", out);
		break;
	case PROC_EXITED:
		if (res->exit_code != 0) {
			/* ffmpeg exits with negative AVERROR codes */
			gc_log(GC_LOG_ERROR, "ffmpeg failed (exit code %ld) cutting %s", (long)res->exit_code, out);
			log_output(GC_LOG_ERROR, res->output);
		} else if (!fs_exists(out)) {
			gc_log(GC_LOG_ERROR, "ffmpeg reported success but %s was not written", out);
			log_output(GC_LOG_ERROR, res->output);
		} else {
			ok = true;
		}
		break;
	}

	if (ok) {
		gc_log(GC_LOG_INFO, "clip written: %s", out);
	} else if (res->status != PROC_START_FAILED && fs_exists(out)) {
		fs_delete(out, NULL); /* partial output of the failed run */
	}
	free(res);
	return ok;
}

static void delete_replay(const char *path, void *cancel_event)
{
	unsigned long err = 0;
	char text[256];

	for (int attempt = 0; attempt < DELETE_ATTEMPTS; attempt++) {
		if (fs_delete(path, &err)) {
			gc_log(GC_LOG_INFO, "deleted the full replay %s", path);
			return;
		}
		if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
			return;
		if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION)
			break;
		/* something (e.g. OBS's automatic remux) still has it open */
		if (!cancel_event)
			Sleep(1000);
		else if (WaitForSingleObject((HANDLE)cancel_event, 1000) == WAIT_OBJECT_0)
			break;
	}
	win_error_text(err, text, sizeof(text));
	gc_log(GC_LOG_WARNING, "could not delete the full replay %s: %s", path, text);
}

bool extract_run_job(const struct extract_job *job, void *cancel_event)
{
	const char *ext = path_extension(job->replay_path);
	size_t ok = 0;

	for (size_t i = 0; i < job->num_clips; i++) {
		if (is_cancelled(cancel_event)) {
			gc_log(GC_LOG_WARNING, "%zu clip(s) from %s were not cut because OBS is closing",
			       job->num_clips - i, job->replay_path);
			break;
		}
		if (cut_clip(job, &job->clips[i], ext, cancel_event))
			ok++;
	}

	if (ok != job->num_clips) {
		gc_log(GC_LOG_WARNING, "kept the full replay %s because %zu of %zu clip(s) failed", job->replay_path,
		       job->num_clips - ok, job->num_clips);
		return false;
	}
	if (!job->keep_full_replay)
		delete_replay(job->replay_path, cancel_event);
	return true;
}

static DWORD WINAPI worker_main(LPVOID param)
{
	struct extract_worker *w = param;

	for (;;) {
		struct extract_job *job;
		HANDLE waits[2];

		EnterCriticalSection((CRITICAL_SECTION *)w->lock);
		job = w->head;
		if (job) {
			w->head = job->next;
			if (!w->head)
				w->tail = NULL;
		}
		LeaveCriticalSection((CRITICAL_SECTION *)w->lock);

		if (job) {
			extract_run_job(job, w->cancel);
			extract_job_free(job);
			continue;
		}

		waits[0] = w->wake;
		waits[1] = w->stop;
		if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
			break; /* stop, and the queue is empty */
	}
	return 0;
}

bool extract_worker_start(struct extract_worker *w)
{
	memset(w, 0, sizeof(*w));
	w->lock = malloc(sizeof(CRITICAL_SECTION));
	if (!w->lock)
		return false;
	InitializeCriticalSection((CRITICAL_SECTION *)w->lock);
	w->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	w->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	w->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (w->wake && w->stop && w->cancel)
		w->thread = CreateThread(NULL, 0, worker_main, w, 0, NULL);
	if (!w->thread) {
		extract_worker_stop(w, 0);
		return false;
	}
	return true;
}

void extract_worker_push(struct extract_worker *w, struct extract_job *job)
{
	job->next = NULL;
	EnterCriticalSection((CRITICAL_SECTION *)w->lock);
	if (w->tail)
		w->tail->next = job;
	else
		w->head = job;
	w->tail = job;
	LeaveCriticalSection((CRITICAL_SECTION *)w->lock);
	SetEvent(w->wake);
}

void extract_worker_stop(struct extract_worker *w, unsigned long grace_ms)
{
	if (w->thread) {
		SetEvent(w->stop);
		if (WaitForSingleObject(w->thread, grace_ms) != WAIT_OBJECT_0) {
			gc_log(GC_LOG_WARNING, "clip cutting is taking too long; cancelling it");
			SetEvent(w->cancel);
			WaitForSingleObject(w->thread, INFINITE);
		}
		CloseHandle(w->thread);
		w->thread = NULL;
	}
	while (w->head) {
		struct extract_job *next = w->head->next;
		extract_job_free(w->head);
		w->head = next;
	}
	w->tail = NULL;
	if (w->wake)
		CloseHandle(w->wake);
	if (w->stop)
		CloseHandle(w->stop);
	if (w->cancel)
		CloseHandle(w->cancel);
	if (w->lock) {
		DeleteCriticalSection((CRITICAL_SECTION *)w->lock);
		free(w->lock);
	}
	memset(w, 0, sizeof(*w));
}
