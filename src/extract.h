/*
 * Cutting clips out of a saved replay with ffmpeg, on a worker thread of its
 * own so neither LiveSplit polling nor OBS's UI thread waits for ffmpeg.
 */
#pragma once

#include "clips.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXTRACT_TIMEOUT_MS 60000

struct extract_job {
	char *replay_path;     /* UTF-8, as OBS reported it */
	int64_t t_req_ns;      /* when the save was requested (plugin clock) */
	double buffer_seconds; /* configured replay buffer length, 0 if unknown */
	char *folder;
	char *ffmpeg;
	bool keep_full_replay;
	struct clip *clips; /* owned */
	size_t num_clips;
	struct extract_job *next;
};

void extract_job_free(struct extract_job *job);

/* Builds the ffmpeg argument list for one cut. num_buf holds the formatted
 * numbers and must outlive argv. Returns argc. */
size_t ffmpeg_clip_args(const char *ffmpeg, const char *input, int64_t sseof_ms, int64_t dur_ms, const char *output,
			char num_buf[2][32], const char *argv[24]);

/* Cuts every clip of the job and deletes the full replay when all succeeded
 * (unless keep_full_replay). Returns true when every clip was written.
 * cancel_event (may be NULL) aborts a running ffmpeg. */
bool extract_run_job(const struct extract_job *job, void *cancel_event);

struct extract_worker {
	void *thread;
	void *wake;   /* auto-reset: a job was queued */
	void *stop;   /* manual-reset: finish queued jobs, then exit */
	void *cancel; /* manual-reset: abort the running ffmpeg */
	void *lock;   /* CRITICAL_SECTION */
	struct extract_job *head;
	struct extract_job *tail;
};

bool extract_worker_start(struct extract_worker *w);
/* Takes ownership of job. */
void extract_worker_push(struct extract_worker *w, struct extract_job *job);
/* Lets queued jobs finish for up to grace_ms, then cancels and joins. */
void extract_worker_stop(struct extract_worker *w, unsigned long grace_ms);
