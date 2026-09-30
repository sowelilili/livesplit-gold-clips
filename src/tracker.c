#include "tracker.h"

#include "gclog.h"
#include "lstime.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESPONSE_SIZE 1024

bool ls_parse_phase(const char *text, enum ls_phase *phase)
{
	if (strcmp(text, "NotRunning") == 0)
		*phase = LS_PHASE_NOT_RUNNING;
	else if (strcmp(text, "Running") == 0)
		*phase = LS_PHASE_RUNNING;
	else if (strcmp(text, "Ended") == 0)
		*phase = LS_PHASE_ENDED;
	else if (strcmp(text, "Paused") == 0)
		*phase = LS_PHASE_PAUSED;
	else
		return false;
	return true;
}

static bool parse_int(const char *text, int *out)
{
	char *end;
	long v;

	while (*text == ' ')
		text++;
	if (!*text)
		return false;
	errno = 0;
	v = strtol(text, &end, 10);
	while (*end == ' ')
		end++;
	if (*end || errno || v < -1000000 || v > 1000000)
		return false;
	*out = (int)v;
	return true;
}

void tracker_init(struct tracker *t, const struct tracker_callbacks *cb)
{
	memset(t, 0, sizeof(*t));
	if (cb)
		t->cb = *cb;
	t->clip_unknown_best = true;
	t->index = -1;
}

void tracker_free(struct tracker *t)
{
	free(t->splits);
	t->splits = NULL;
	t->splits_cap = 0;
}

static bool ensure_splits(struct tracker *t, int n)
{
	struct split_state *p;
	int cap;

	if (n <= t->splits_cap)
		return true;
	cap = t->splits_cap ? t->splits_cap : 32;
	while (cap < n)
		cap *= 2;
	p = realloc(t->splits, (size_t)cap * sizeof(*p));
	if (!p)
		return false;
	memset(p + t->splits_cap, 0, (size_t)(cap - t->splits_cap) * sizeof(*p));
	t->splits = p;
	t->splits_cap = cap;
	return true;
}

static void end_run(struct tracker *t, bool connection_lost)
{
	if (!t->run_active)
		return;
	t->run_active = false;
	t->index = -1;
	if (connection_lost)
		gc_log(GC_LOG_INFO, "run %llu: lost track of the run (connection to LiveSplit lost)",
		       (unsigned long long)t->run_id);
	else
		gc_log(GC_LOG_INFO, "run %llu: reset", (unsigned long long)t->run_id);
	if (t->cb.run_ended)
		t->cb.run_ended(t->cb.ctx, t->run_id, connection_lost);
}

void tracker_connection_lost(struct tracker *t)
{
	end_run(t, true);
	t->seen_not_running = false;
}

static int start_run(struct tracker *t, int64_t now, int index, ls_query_fn q, void *qc)
{
	char buf[RESPONSE_SIZE];
	int count = 0;
	struct run_info info;

	if (!q(qc, "getgamename", buf, sizeof(buf)))
		return TRACKER_IO_ERROR;
	snprintf(t->game, sizeof(t->game), "%s", buf);
	if (!q(qc, "getcategoryname", buf, sizeof(buf)))
		return TRACKER_IO_ERROR;
	snprintf(t->category, sizeof(t->category), "%s", buf);
	if (!q(qc, "getsplitcount", buf, sizeof(buf)))
		return TRACKER_IO_ERROR;
	if (!parse_int(buf, &count) || count < 0) {
		gc_log(GC_LOG_WARNING, "unexpected split count \"%s\"", buf);
		count = 0;
	}

	if (!ensure_splits(t, (count > index ? count : index) + 1))
		return TRACKER_NO_MEMORY;
	memset(t->splits, 0, (size_t)t->splits_cap * sizeof(*t->splits));

	t->run_active = true;
	t->run_id++;
	t->count = count;
	t->index = index;
	t->start_known = t->seen_not_running && index == 0;
	t->start_ns = now;

	if (t->start_known)
		gc_log(GC_LOG_INFO, "run %llu started: %s / %s (%d splits)", (unsigned long long)t->run_id, t->game,
		       t->category, count);
	else
		gc_log(GC_LOG_INFO,
		       "run %llu already in progress at split %d: %s / %s (%d splits); "
		       "the current segment cannot be clipped",
		       (unsigned long long)t->run_id, index + 1, t->game, t->category, count);

	info.run_id = t->run_id;
	info.game = t->game;
	info.category = t->category;
	info.split_count = count;
	info.first_index = index;
	info.start_observed = t->start_known;
	if (t->cb.run_started)
		t->cb.run_started(t->cb.ctx, &info);
	return TRACKER_OK;
}

/* Checks split j, which just happened, and reports it if it is a gold. */
static int evaluate_split(struct tracker *t, int j, ls_query_fn q, void *qc)
{
	const struct split_state *sp = &t->splits[j];
	const struct split_state *prev = j > 0 ? &t->splits[j - 1] : NULL;
	int64_t seg, best = 0;
	bool gold, has_best = false;
	char name[RESPONSE_SIZE], cmd[64], seg_text[32], best_text[32];
	struct gold_event ev;
	int result = TRACKER_OK;

	if (!sp->t_known) {
		gc_log(GC_LOG_DEBUG, "split %d has no time (skipped)", j + 1);
		return TRACKER_OK;
	}
	if (prev && !prev->t_known) {
		gc_log(GC_LOG_DEBUG, "split %d: previous split time unknown, not checked", j + 1);
		return TRACKER_OK;
	}
	seg = sp->t - (prev ? prev->t : 0);

	if (!sp->bs_queried) {
		gc_log(GC_LOG_DEBUG, "split %d: best segment unknown, not checked", j + 1);
		return TRACKER_OK;
	}
	if (sp->bs_known) {
		if (prev && !(prev->bs_queried && prev->bs_known)) {
			gc_log(GC_LOG_DEBUG, "split %d: previous best segment total unknown, not checked", j + 1);
			return TRACKER_OK;
		}
		best = sp->bs - (prev ? prev->bs : 0);
		has_best = true;
		gold = seg < best;
	} else {
		gold = t->clip_unknown_best;
	}
	if (!gold)
		return TRACKER_OK;

	snprintf(cmd, sizeof(cmd), "getsplitname %d", j);
	if (!q(qc, cmd, name, sizeof(name))) {
		snprintf(name, sizeof(name), "Split %d", j + 1);
		result = TRACKER_IO_ERROR;
	}

	memset(&ev, 0, sizeof(ev));
	ev.run_id = t->run_id;
	ev.split_index = j;
	ev.split_name = name;
	ev.game = t->game;
	ev.category = t->category;
	ev.seg_ticks = seg;
	ev.has_best = has_best;
	ev.best_ticks = best;
	ev.seg_end_ns = sp->wall;
	if (j == 0) {
		ev.window_known = t->start_known;
		ev.seg_start_ns = t->start_ns;
	} else {
		ev.window_known = prev->wall_known;
		ev.seg_start_ns = prev->wall;
	}

	lstime_format_display(seg, seg_text, sizeof(seg_text));
	if (has_best)
		lstime_format_display(best, best_text, sizeof(best_text));
	else
		snprintf(best_text, sizeof(best_text), "none");
	gc_log(GC_LOG_INFO, "GOLD on split %d \"%s\": %s (previous best %s)", j + 1, name, seg_text, best_text);

	if (t->cb.gold)
		t->cb.gold(t->cb.ctx, &ev);
	return result;
}

static int handle_splits(struct tracker *t, int64_t now, int index, ls_query_fn q, void *qc)
{
	char buf[RESPONSE_SIZE];
	int from = t->index;
	int last = index - 1;
	struct split_state *sp;

	if (!ensure_splits(t, index + 1))
		return TRACKER_NO_MEMORY;
	if (!q(qc, "getlastsplittime", buf, sizeof(buf)))
		return TRACKER_IO_ERROR;

	/* Splits passed over in the same poll: their times are not known. */
	for (int k = from; k < last; k++) {
		t->splits[k].t_known = false;
		t->splits[k].wall_known = false;
	}
	if (index - from > 1)
		gc_log(GC_LOG_INFO, "splits %d to %d happened within one poll; only split %d can be checked",
		       from + 1, index, index);

	sp = &t->splits[last];
	sp->t_known = lstime_parse(buf, &sp->t);
	sp->wall_known = true;
	sp->wall = now;
	t->index = index;

	return evaluate_split(t, last, q, qc);
}

static void handle_undo(struct tracker *t, int index)
{
	int old = t->index;

	for (int k = index; k < old; k++) {
		t->splits[k].t_known = false;
		t->splits[k].wall_known = false;
	}
	t->index = index;
	gc_log(GC_LOG_INFO, "run %llu: undo back to split %d", (unsigned long long)t->run_id, index + 1);
	if (t->cb.undo)
		t->cb.undo(t->cb.ctx, t->run_id, index, old);
}

int tracker_poll(struct tracker *t, int64_t now, ls_query_fn q, void *qc)
{
	char buf[RESPONSE_SIZE];
	enum ls_phase phase;
	int index, r;

	if (!q(qc, "getcurrenttimerphase", buf, sizeof(buf)))
		return TRACKER_IO_ERROR;
	if (!ls_parse_phase(buf, &phase)) {
		gc_log(GC_LOG_WARNING, "unexpected timer phase \"%s\"", buf);
		return TRACKER_PROTOCOL_ERROR;
	}
	if (!q(qc, "getsplitindex", buf, sizeof(buf)))
		return TRACKER_IO_ERROR;
	if (!parse_int(buf, &index)) {
		gc_log(GC_LOG_WARNING, "unexpected split index \"%s\"", buf);
		return TRACKER_PROTOCOL_ERROR;
	}

	if (phase == LS_PHASE_NOT_RUNNING || index < 0) {
		end_run(t, false);
		t->seen_not_running = true;
		return TRACKER_OK;
	}

	if (!t->run_active) {
		r = start_run(t, now, index, q, qc);
	} else if (index > t->index) {
		r = handle_splits(t, now, index, q, qc);
	} else {
		if (index < t->index)
			handle_undo(t, index);
		r = TRACKER_OK;
	}
	if (r != TRACKER_OK)
		return r;

	/* Entering a segment: remember the Best Segments total up to it. LiveSplit
	 * only updates best segments when a run resets or ends, so the value
	 * holds for the whole run. */
	if (t->index < t->count && !t->splits[t->index].bs_queried) {
		struct split_state *sp = &t->splits[t->index];
		if (!q(qc, "getcomparisonsplittime Best Segments", buf, sizeof(buf)))
			return TRACKER_IO_ERROR;
		sp->bs_queried = true;
		sp->bs_known = lstime_parse(buf, &sp->bs);
	}
	return TRACKER_OK;
}
