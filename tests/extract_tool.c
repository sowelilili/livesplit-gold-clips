/*
 * Runs the plugin's real extraction code (extract_run_job) on a file, as if
 * OBS had saved it as a replay at t_req:
 *
 *   extract_tool <replay> <folder> <keep_full 0|1> <ffmpeg> <buffer_seconds>
 *                [<game> <category> <split> <seg_seconds> <start_before_end> <length>]...
 *
 * Each clip window starts start_before_end seconds before the end of the
 * replay and is length seconds long. Arguments are read as UTF-16 and passed
 * on as UTF-8, like OBS paths. Exit code 0 when every clip was written.
 */
#include "clips.h"
#include "extract.h"
#include "gclog.h"
#include "winutil.h"

#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_PER_SECOND 1000000000LL

static int64_t seconds_ns(const char *s)
{
	return (int64_t)(atof(s) * (double)NS_PER_SECOND + 0.5);
}

int main(void)
{
	int argc = 0;
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
	char **argv;
	struct extract_job *job;
	bool ok;

	if (!wargv || argc < 6 || (argc - 6) % 6 != 0) {
		fprintf(stderr, "usage: extract_tool <replay> <folder> <keep_full 0|1> <ffmpeg> <buffer_seconds> "
				"[<game> <category> <split> <seg_seconds> <start_before_end> <length>]...\n");
		return 2;
	}
	argv = calloc((size_t)argc, sizeof(char *));
	for (int i = 0; i < argc; i++)
		argv[i] = wide_to_utf8(wargv[i]);

	job = calloc(1, sizeof(*job));
	job->replay_path = _strdup(argv[1]);
	job->folder = _strdup(argv[2]);
	job->keep_full_replay = atoi(argv[3]) != 0;
	job->ffmpeg = _strdup(argv[4]);
	job->buffer_seconds = atof(argv[5]);
	job->t_req_ns = 1000 * NS_PER_SECOND;
	job->num_clips = (size_t)(argc - 6) / 6;
	job->clips = calloc(job->num_clips ? job->num_clips : 1, sizeof(struct clip));

	for (size_t i = 0; i < job->num_clips; i++) {
		char **a = argv + 6 + i * 6;
		struct clip *c = &job->clips[i];
		c->run_id = 1;
		c->split_index = (int)i;
		c->game = _strdup(a[0]);
		c->category = _strdup(a[1]);
		c->split_name = _strdup(a[2]);
		c->seg_ticks = (int64_t)(atof(a[3]) * 10000000.0 + 0.5);
		c->start_ns = job->t_req_ns - seconds_ns(a[4]);
		c->end_ns = c->start_ns + seconds_ns(a[5]);
	}

	ok = extract_run_job(job, NULL);
	extract_job_free(job);
	for (int i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);
	LocalFree(wargv);
	return ok ? 0 : 1;
}
