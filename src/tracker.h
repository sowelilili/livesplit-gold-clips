/*
 * Gold split detection. The tracker is fed by a poller: every poll it asks
 * LiveSplit for the timer phase and split index through a query callback and
 * turns changes into events (run started/ended, gold, undo). It owns no I/O,
 * so tests drive it with a scripted fake LiveSplit.
 *
 * Times: split times are LiveSplit TimeSpan ticks (100 ns); wall times are
 * nanoseconds of whatever monotonic clock the caller passes as "now".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum ls_phase {
	LS_PHASE_NOT_RUNNING,
	LS_PHASE_RUNNING,
	LS_PHASE_ENDED,
	LS_PHASE_PAUSED,
};

bool ls_parse_phase(const char *text, enum ls_phase *phase);

/* Sends one command and stores the response line (no newline) in out.
 * Returns false on an I/O failure or timeout. */
typedef bool (*ls_query_fn)(void *ctx, const char *cmd, char *out, size_t out_size);

struct run_info {
	uint64_t run_id;
	const char *game;
	const char *category;
	int split_count;
	int first_index;     /* split index when the run was first seen */
	bool start_observed; /* the NotRunning -> running transition was seen */
};

struct gold_event {
	uint64_t run_id;
	int split_index;
	const char *split_name;
	const char *game;
	const char *category;
	int64_t seg_ticks;   /* this run's segment time */
	bool has_best;       /* false: the split had no best segment yet */
	int64_t best_ticks;  /* previous best segment */
	bool window_known;   /* wall times of the segment start and end are known */
	int64_t seg_start_ns;
	int64_t seg_end_ns;
};

struct tracker_callbacks {
	void *ctx;
	void (*run_started)(void *ctx, const struct run_info *run);
	void (*run_ended)(void *ctx, uint64_t run_id, bool connection_lost);
	void (*gold)(void *ctx, const struct gold_event *ev);
	/* Splits from_split .. to_split-1 were undone. */
	void (*undo)(void *ctx, uint64_t run_id, int from_split, int to_split);
};

struct split_state {
	bool t_known; /* cumulative split time from getlastsplittime */
	int64_t t;
	bool wall_known; /* when the split happened (plugin clock) */
	int64_t wall;
	bool bs_queried; /* "Best Segments" comparison, cumulative */
	bool bs_known;
	int64_t bs;
};

struct tracker {
	struct tracker_callbacks cb;
	bool clip_unknown_best;

	bool seen_not_running; /* since the current connection was made */
	bool run_active;
	uint64_t run_id;
	bool start_known;
	int64_t start_ns;
	int index; /* last seen split index */
	int count;
	char game[512];
	char category[512];
	struct split_state *splits;
	int splits_cap;
};

enum {
	TRACKER_OK = 0,
	TRACKER_IO_ERROR = -1,
	TRACKER_PROTOCOL_ERROR = -2,
	TRACKER_NO_MEMORY = -3,
};

void tracker_init(struct tracker *t, const struct tracker_callbacks *cb);
void tracker_free(struct tracker *t);

/* One poll. Any non-zero result means the connection should be dropped and
 * tracker_connection_lost() called. */
int tracker_poll(struct tracker *t, int64_t now_ns, ls_query_fn query, void *query_ctx);

/* Ends an active run (queued clips are the caller's and stay queued). After a
 * reconnect a running timer counts as a run whose start was not observed. */
void tracker_connection_lost(struct tracker *t);
