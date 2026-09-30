/*
 * Clips waiting for a replay buffer save, plus the pure helpers that turn a
 * clip into an ffmpeg cut and an output path. Nothing here touches OBS or the
 * file system (existence checks are passed in).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GC_MAX_COMPONENT_BYTES 100

struct clip {
	uint64_t run_id;
	int split_index;
	char *game;
	char *category;
	char *split_name;
	int64_t seg_ticks; /* LiveSplit segment time, used in the file name */
	int64_t start_ns;  /* clip window on the plugin clock */
	int64_t end_ns;
};

bool clip_copy(struct clip *dst, const struct clip *src);
void clip_free(struct clip *c);
void clip_free_array(struct clip *clips, size_t num);

struct clipq {
	struct clip *items;
	size_t num;
	size_t cap;
};

void clipq_init(struct clipq *q);
void clipq_free(struct clipq *q);
bool clipq_add(struct clipq *q, const struct clip *c); /* deep copy */
/* Removes clips of run_id for splits >= from_split; returns how many. */
size_t clipq_cancel(struct clipq *q, uint64_t run_id, int from_split);
bool clipq_latest_end(const struct clipq *q, int64_t *end_ns);
/* Moves every clip whose window ends at or before t_ns into a new array
 * (*out, free with clip_free_array; NULL when none). Returns the count. */
size_t clipq_take_ended_by(struct clipq *q, int64_t t_ns, struct clip **out);

/* [segment start - lead, segment end + tail] */
void clip_window(int64_t seg_start_ns, int64_t seg_end_ns, double lead_seconds, double tail_seconds,
		 int64_t *start_ns, int64_t *end_ns);

/* ffmpeg cut for a clip from a replay saved at t_req_ns: start sseof_ms before
 * the end of the file, keep dur_ms. When buffer_seconds > 0 and the window
 * reaches further back than that, the start is moved up to the buffer's
 * start and the duration shortened to keep the end in place (returns true).
 * dur_ms <= 0 means the whole window is older than the buffer. */
bool clip_cut_params(int64_t t_req_ns, int64_t start_ns, int64_t end_ns, double buffer_seconds, int64_t *sseof_ms,
		     int64_t *dur_ms);

/* Makes one path component safe on Windows: replaces \ / : * ? " < > | and
 * control characters with '_', trims leading spaces and trailing dots and
 * spaces, prefixes reserved device names (CON, NUL, COM1, ...) with '_',
 * caps the length at GC_MAX_COMPONENT_BYTES without splitting UTF-8
 * sequences, and turns an empty result into "_". */
void path_sanitize_component(const char *in, char *out, size_t out_size);

/* ".mkv" style extension of the last path component, or "" */
const char *path_extension(const char *path);

/* <directory of replay>/<folder parts>/<game>/<category>, each part sanitized.
 * folder may contain '/' or '\' separated parts; an empty folder adds none. */
bool clip_output_dir(const char *replay_path, const char *folder, const char *game, const char *category, char *out,
		     size_t out_size);

/* "<split> <seg time>", e.g. "Level 1 1m23.45s" */
void clip_file_base(const char *split_name, int64_t seg_ticks, char *out, size_t out_size);

typedef bool (*path_exists_fn)(void *ctx, const char *path);

/* dir/base+ext, or dir/base (2)+ext, (3), ... whichever does not exist yet. */
bool clip_unique_path(const char *dir, const char *base, const char *ext, path_exists_fn exists, void *ctx,
		      char *out, size_t out_size);
