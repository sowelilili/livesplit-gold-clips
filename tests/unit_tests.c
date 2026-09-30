/*
 * Unit tests for the OBS-free parts: LiveSplit time parsing and formatting,
 * path sanitizing and naming, the clip queue, the gold tracker (driven by a
 * scripted in-memory LiveSplit), command line quoting and the process runner.
 */
#include "clips.h"
#include "extract.h"
#include "gclog.h"
#include "lstime.h"
#include "procrun.h"
#include "tracker.h"
#include "winutil.h"

#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks_run, checks_failed, cases_run;
static const char *current_case;

static void check(bool ok, const char *expr, int line)
{
	checks_run++;
	if (!ok) {
		checks_failed++;
		printf("FAIL [%s] line %d: %s\n", current_case, line, expr);
	}
}

static void check_str(const char *got, const char *want, const char *expr, int line)
{
	checks_run++;
	if (strcmp(got, want) != 0) {
		checks_failed++;
		printf("FAIL [%s] line %d: %s is \"%s\", expected \"%s\"\n", current_case, line, expr, got, want);
	}
}

static void check_i64(long long got, long long want, const char *expr, int line)
{
	checks_run++;
	if (got != want) {
		checks_failed++;
		printf("FAIL [%s] line %d: %s is %lld, expected %lld\n", current_case, line, expr, got, want);
	}
}

#define CHECK(c) check((c), #c, __LINE__)
#define CHECK_STR(a, b) check_str((a), (b), #a, __LINE__)
#define CHECK_I64(a, b) check_i64((long long)(a), (long long)(b), #a, __LINE__)

#define SEC(s) ((int64_t)((s) * 10000000.0 + 0.5)) /* seconds -> LiveSplit ticks */
#define MS(ms) ((int64_t)(ms) * 1000000LL)        /* milliseconds -> ns */

static void quiet_sink(int level, const char *msg)
{
	(void)level;
	(void)msg;
}

/* ------------------------------------------------------------------------ */
/* LiveSplit times                                                          */

static bool parse(const char *s, int64_t *t)
{
	*t = 12345;
	return lstime_parse(s, t);
}

static void test_lstime_parse(void)
{
	int64_t t;

	CHECK(parse("00:01:23.4560000", &t));
	CHECK_I64(t, SEC(83.456));
	CHECK(parse("1.02:03:04.5000000", &t));
	CHECK_I64(t, SEC(((24 + 2) * 60 + 3) * 60 + 4.5));
	CHECK(parse("-00:00:01.5000000", &t));
	CHECK_I64(t, -SEC(1.5));
	CHECK(parse("00:00:05", &t));
	CHECK_I64(t, SEC(5));
	CHECK(parse("1:23.45", &t));
	CHECK_I64(t, SEC(83.45));
	CHECK(parse("23.45", &t));
	CHECK_I64(t, SEC(23.45));
	CHECK(parse("12:34", &t));
	CHECK_I64(t, SEC(12 * 60 + 34));
	CHECK(parse("0:00:00.12345678", &t)); /* more than 7 digits: truncated */
	CHECK_I64(t, 1234567);
	CHECK(parse(" 00:00:01.0000000\r\n", &t));
	CHECK_I64(t, SEC(1));
	CHECK(parse("00:00:02,5000000", &t)); /* comma decimal separator */
	CHECK_I64(t, SEC(2.5));
	CHECK(parse("25:00:00", &t)); /* hours past a day without the d. form */
	CHECK_I64(t, SEC(25 * 3600));

	CHECK(!parse("-", &t));
	CHECK(!parse("", &t));
	CHECK(!parse("   ", &t));
	CHECK(!parse(NULL, &t));
	CHECK(!parse("abc", &t));
	CHECK(!parse("1:2:3:4", &t));
	CHECK(!parse("1:ab", &t));
	CHECK(!parse(".5", &t));
	CHECK(!parse("1.", &t));
	CHECK(!parse("1:23,", &t));
	CHECK(!parse("--1", &t));
	CHECK(!parse("1 2", &t));
	CHECK(!parse("99999999999999:00", &t)); /* absurdly long field */
}

static void test_lstime_format(void)
{
	char buf[64];

	lstime_format_filename(SEC(23.456789), buf, sizeof(buf));
	CHECK_STR(buf, "23.45s");
	lstime_format_filename(SEC(83.45), buf, sizeof(buf));
	CHECK_STR(buf, "1m23.45s");
	lstime_format_filename(SEC(63.05), buf, sizeof(buf));
	CHECK_STR(buf, "1m03.05s");
	lstime_format_filename(SEC(3723.45), buf, sizeof(buf));
	CHECK_STR(buf, "1h02m03.45s");
	lstime_format_filename(0, buf, sizeof(buf));
	CHECK_STR(buf, "0.00s");
	lstime_format_filename(SEC(59.999), buf, sizeof(buf));
	CHECK_STR(buf, "59.99s");
	lstime_format_filename(SEC(60), buf, sizeof(buf));
	CHECK_STR(buf, "1m00.00s");
	lstime_format_filename(-SEC(1.5), buf, sizeof(buf));
	CHECK_STR(buf, "-1.50s");
	lstime_format_filename(SEC(25 * 3600), buf, sizeof(buf));
	CHECK_STR(buf, "25h00m00.00s");

	lstime_format_display(SEC(23.456), buf, sizeof(buf));
	CHECK_STR(buf, "23.45");
	lstime_format_display(SEC(83.45), buf, sizeof(buf));
	CHECK_STR(buf, "1:23.45");
	lstime_format_display(SEC(3723.45), buf, sizeof(buf));
	CHECK_STR(buf, "1:02:03.45");
}

/* ------------------------------------------------------------------------ */
/* paths and names                                                          */

static const char *san(const char *in)
{
	static char out[GC_MAX_COMPONENT_BYTES + 8];
	path_sanitize_component(in, out, sizeof(out));
	return out;
}

static void test_sanitize(void)
{
	char longname[256], expected[256];

	CHECK_STR(san("a\\b/c:d*e?f\"g<h>i|j"), "a_b_c_d_e_f_g_h_i_j");
	CHECK_STR(san("Ratchet & Clank: Going Commando"), "Ratchet & Clank_ Going Commando");
	CHECK_STR(san("Any% (No QE)"), "Any% (No QE)");
	CHECK_STR(san("name. . "), "name");
	CHECK_STR(san("  leading"), "leading");
	CHECK_STR(san("tab\there\x01"), "tab_here_");
	CHECK_STR(san(""), "_");
	CHECK_STR(san(NULL), "_");
	CHECK_STR(san("..."), "_");
	CHECK_STR(san(".."), "_");
	CHECK_STR(san("CON"), "_CON");
	CHECK_STR(san("con"), "_con");
	CHECK_STR(san("con.txt"), "_con.txt");
	CHECK_STR(san("NUL .mkv"), "_NUL .mkv");
	CHECK_STR(san("COM1"), "_COM1");
	CHECK_STR(san("lpt9.log"), "_lpt9.log");
	CHECK_STR(san("AUX."), "_AUX");
	CHECK_STR(san("CONSOLE"), "CONSOLE");
	CHECK_STR(san("COMA"), "COMA");
	CHECK_STR(san("Gr\xC3\xB6\xC3\x9F" "e"), "Gr\xC3\xB6\xC3\x9F" "e");
	CHECK_STR(san("\xE6\x97\xA5\xE6\x9C\xAC"), "\xE6\x97\xA5\xE6\x9C\xAC");
	CHECK_STR(san("bad\xFFutf8\xC3"), "bad_utf8_");

	/* 60 x "é" (2 bytes each): the cap of 100 bytes keeps 50 whole characters */
	longname[0] = expected[0] = '\0';
	for (int i = 0; i < 60; i++)
		strcat(longname, "\xC3\xA9");
	for (int i = 0; i < 50; i++)
		strcat(expected, "\xC3\xA9");
	CHECK_STR(san(longname), expected);
	CHECK_I64(strlen(san(longname)), 100);
	/* an odd cap position must not split a character */
	memset(longname, 'a', 99);
	longname[99] = '\0';
	strcat(longname, "\xC3\xA9xyz");
	CHECK_I64(strlen(san(longname)), 99);
}

static void test_paths(void)
{
	char out[512];

	CHECK_STR(path_extension("C:/rec/Replay 1.mkv"), ".mkv");
	CHECK_STR(path_extension("C:/a.b/replay"), "");
	CHECK_STR(path_extension("C:\\a.b\\x.mp4"), ".mp4");
	CHECK_STR(path_extension("noext"), "");
	CHECK_STR(path_extension("C:/rec/.hidden"), "");

	CHECK(clip_output_dir("C:/rec/Replay 1.mkv", "Golds", "G: X", "Any%", out, sizeof(out)));
	CHECK_STR(out, "C:/rec/Golds/G_ X/Any%");
	CHECK(clip_output_dir("C:\\rec\\r.mkv", "a/b\\c", "Game", "Cat", out, sizeof(out)));
	CHECK_STR(out, "C:\\rec/a/b/c/Game/Cat");
	CHECK(clip_output_dir("C:/rec/r.mkv", "", "Game", "Cat", out, sizeof(out)));
	CHECK_STR(out, "C:/rec/Game/Cat");
	CHECK(clip_output_dir("C:/rec/r.mkv", "../x/./y:", "", "CON", out, sizeof(out)));
	CHECK_STR(out, "C:/rec/_/x/y_/_/_CON");
	CHECK(clip_output_dir("r.mkv", "Golds", "g", "c", out, sizeof(out)));
	CHECK_STR(out, "./Golds/g/c");
	CHECK(!clip_output_dir("C:/rec/r.mkv", "Golds", "g", "c", out, 12));

	clip_file_base("Level 1", SEC(83.45), out, sizeof(out));
	CHECK_STR(out, "Level 1 1m23.45s");
	clip_file_base("Boss? <Fast>", SEC(9.999), out, sizeof(out));
	CHECK_STR(out, "Boss_ _Fast_ 9.99s");
	clip_file_base("", SEC(1), out, sizeof(out));
	CHECK_STR(out, "_ 1.00s");
	clip_file_base("NUL", SEC(1), out, sizeof(out));
	CHECK_STR(out, "_NUL 1.00s");
}

struct fake_fs {
	const char *existing[4];
};

static bool fake_exists(void *ctx, const char *path)
{
	struct fake_fs *fs = ctx;
	for (int i = 0; i < 4; i++)
		if (fs->existing[i] && strcmp(fs->existing[i], path) == 0)
			return true;
	return false;
}

static void test_unique_path(void)
{
	char out[256];
	struct fake_fs none = {{NULL}};
	struct fake_fs two = {{"d/a 1.00s.mkv", "d/a 1.00s (2).mkv", NULL}};

	CHECK(clip_unique_path("d", "a 1.00s", ".mkv", fake_exists, &none, out, sizeof(out)));
	CHECK_STR(out, "d/a 1.00s.mkv");
	CHECK(clip_unique_path("d", "a 1.00s", ".mkv", fake_exists, &two, out, sizeof(out)));
	CHECK_STR(out, "d/a 1.00s (3).mkv");
	CHECK(!clip_unique_path("d", "a 1.00s", ".mkv", fake_exists, &none, out, 5));
}

/* ------------------------------------------------------------------------ */
/* clip windows, cuts and the queue                                         */

static void test_cut_params(void)
{
	int64_t start, end, x, d;

	clip_window(MS(10000), MS(15000), 2.0, 2.5, &start, &end);
	CHECK_I64(start, MS(8000));
	CHECK_I64(end, MS(17500));

	/* saved at 20 s: start 12 s before the end, 9.5 s long */
	CHECK(!clip_cut_params(MS(20000), start, end, 60, &x, &d));
	CHECK_I64(x, 12000);
	CHECK_I64(d, 9500);
	/* unknown buffer length: no clamping */
	CHECK(!clip_cut_params(MS(20000), start, end, 0, &x, &d));
	CHECK_I64(x, 12000);
	/* 10 s buffer: start at the buffer start, end unchanged */
	CHECK(clip_cut_params(MS(20000), start, end, 10, &x, &d));
	CHECK_I64(x, 10000);
	CHECK_I64(d, 7500);
	/* entirely older than a 2 s buffer */
	CHECK(clip_cut_params(MS(20000), start, end, 2, &x, &d));
	CHECK(d <= 0);
	/* sub-millisecond values round up so the start is never late */
	CHECK(!clip_cut_params(MS(20000) + 1, MS(10000), MS(15000) + 1, 0, &x, &d));
	CHECK_I64(x, 10001);
	CHECK_I64(d, 5001);
}

static struct clip mkclip(uint64_t run, int split, const char *name, int64_t start_ms, int64_t end_ms)
{
	struct clip c;
	memset(&c, 0, sizeof(c));
	c.run_id = run;
	c.split_index = split;
	c.game = "Game";
	c.category = "Cat";
	c.split_name = (char *)name;
	c.seg_ticks = SEC(1);
	c.start_ns = MS(start_ms);
	c.end_ns = MS(end_ms);
	return c;
}

static void test_clipq(void)
{
	struct clipq q;
	struct clip c, *taken;
	int64_t latest;
	size_t n;

	clipq_init(&q);
	CHECK(!clipq_latest_end(&q, &latest));
	c = mkclip(1, 0, "a", 0, 1000);
	CHECK(clipq_add(&q, &c));
	c = mkclip(1, 2, "b", 2000, 5000);
	CHECK(clipq_add(&q, &c));
	c = mkclip(2, 0, "c", 6000, 7000);
	CHECK(clipq_add(&q, &c));
	c = mkclip(1, 3, "d", 5000, 6000);
	CHECK(clipq_add(&q, &c));
	CHECK(clipq_latest_end(&q, &latest));
	CHECK_I64(latest, MS(7000));

	/* undo in run 1 back to split 2 cancels "b" and "d", not run 2's clip */
	CHECK_I64(clipq_cancel(&q, 1, 2), 2);
	CHECK_I64(q.num, 2);
	CHECK_STR(q.items[0].split_name, "a");
	CHECK_STR(q.items[1].split_name, "c");

	n = clipq_take_ended_by(&q, MS(1000), &taken);
	CHECK_I64(n, 1);
	CHECK_STR(taken[0].split_name, "a");
	clip_free_array(taken, n);
	CHECK_I64(q.num, 1);
	n = clipq_take_ended_by(&q, MS(6999), &taken);
	CHECK_I64(n, 0);
	CHECK(taken == NULL);
	clipq_free(&q);
}

/* ------------------------------------------------------------------------ */
/* gold tracker against a scripted LiveSplit                                */

#define MAX_SPLITS 8

struct fake_ls {
	const char *phase;
	int index;
	int count;
	const char *game;
	const char *category;
	const char *names[MAX_SPLITS];
	const char *bs[MAX_SPLITS]; /* Best Segments comparison, cumulative */
	char times[MAX_SPLITS][40];
	bool fail;
	int bad_commands;
};

static void ticks_text(int64_t t, char *buf, size_t size)
{
	long long s = (long long)(t / 10000000), f = (long long)(t % 10000000);
	snprintf(buf, size, "%02lld:%02lld:%02lld.%07lld", s / 3600, (s / 60) % 60, s % 60, f);
}

static bool fake_query(void *ctx, const char *cmd, char *out, size_t size)
{
	struct fake_ls *f = ctx;

	if (f->fail)
		return false;
	if (strcmp(cmd, "getcurrenttimerphase") == 0) {
		snprintf(out, size, "%s", f->phase);
	} else if (strcmp(cmd, "getsplitindex") == 0) {
		snprintf(out, size, "%d", f->index);
	} else if (strcmp(cmd, "getsplitcount") == 0) {
		snprintf(out, size, "%d", f->count);
	} else if (strcmp(cmd, "getgamename") == 0) {
		snprintf(out, size, "%s", f->game);
	} else if (strcmp(cmd, "getcategoryname") == 0) {
		snprintf(out, size, "%s", f->category);
	} else if (strncmp(cmd, "getsplitname ", 13) == 0) {
		int i = atoi(cmd + 13);
		if (i < 0 || i >= f->count)
			f->bad_commands++;
		snprintf(out, size, "%s", i >= 0 && i < f->count ? f->names[i] : "");
	} else if (strcmp(cmd, "getlastsplittime") == 0) {
		snprintf(out, size, "%s", f->index > 0 ? f->times[f->index - 1] : "-");
	} else if (strcmp(cmd, "getcomparisonsplittime Best Segments") == 0) {
		if (f->index < 0 || f->index >= f->count) {
			f->bad_commands++; /* LiveSplit has no current split then */
			return false;
		}
		snprintf(out, size, "%s", f->bs[f->index]);
	} else {
		f->bad_commands++;
		return false;
	}
	return true;
}

static void fl_init(struct fake_ls *f, int count, const char *const *bs)
{
	static const char *names[MAX_SPLITS] = {"Level 1", "Level 2", "Boss: 3", "Level 4",
						"Level 5", "Level 6", "Level 7", "End"};
	memset(f, 0, sizeof(*f));
	f->phase = "NotRunning";
	f->index = -1;
	f->count = count;
	f->game = "Test Game";
	f->category = "Any%";
	for (int i = 0; i < count; i++) {
		f->names[i] = names[i];
		f->bs[i] = bs ? bs[i] : "-";
	}
	if (count > 0)
		f->names[count - 1] = "End";
}

static void fl_start(struct fake_ls *f)
{
	f->phase = "Running";
	f->index = 0;
	memset(f->times, 0, sizeof(f->times));
}

/* seconds < 0: skip */
static void fl_split(struct fake_ls *f, double seconds)
{
	if (seconds < 0)
		strcpy(f->times[f->index], "-");
	else
		ticks_text(SEC(seconds), f->times[f->index], sizeof(f->times[0]));
	f->index++;
	if (f->index == f->count)
		f->phase = "Ended";
}

static void fl_undo(struct fake_ls *f)
{
	f->index--;
	f->times[f->index][0] = '\0';
	f->phase = "Running";
}

static void fl_reset(struct fake_ls *f)
{
	f->phase = "NotRunning";
	f->index = -1;
}

struct gold_rec {
	uint64_t run;
	int split;
	char name[64];
	int64_t seg, best;
	bool has_best, window_known;
	int64_t start, end;
};

struct rec {
	int started, ended, ended_lost, undos;
	struct run_info run;
	struct gold_rec golds[16];
	int num_golds;
	int undo_from, undo_to;
	size_t cancelled;
	struct clipq q;
	double lead, tail;
};

static void rec_run_started(void *ctx, const struct run_info *run)
{
	struct rec *r = ctx;
	r->started++;
	r->run = *run;
	r->run.game = r->run.category = NULL;
}

static void rec_run_ended(void *ctx, uint64_t run_id, bool lost)
{
	struct rec *r = ctx;
	(void)run_id;
	r->ended++;
	if (lost)
		r->ended_lost++;
}

static void rec_gold(void *ctx, const struct gold_event *ev)
{
	struct rec *r = ctx;
	struct gold_rec *g = &r->golds[r->num_golds++];
	struct clip c;

	g->run = ev->run_id;
	g->split = ev->split_index;
	snprintf(g->name, sizeof(g->name), "%s", ev->split_name);
	g->seg = ev->seg_ticks;
	g->best = ev->best_ticks;
	g->has_best = ev->has_best;
	g->window_known = ev->window_known;
	g->start = ev->seg_start_ns;
	g->end = ev->seg_end_ns;

	if (ev->window_known) {
		memset(&c, 0, sizeof(c));
		c.run_id = ev->run_id;
		c.split_index = ev->split_index;
		c.game = (char *)ev->game;
		c.category = (char *)ev->category;
		c.split_name = (char *)ev->split_name;
		c.seg_ticks = ev->seg_ticks;
		clip_window(ev->seg_start_ns, ev->seg_end_ns, r->lead, r->tail, &c.start_ns, &c.end_ns);
		clipq_add(&r->q, &c);
	}
}

static void rec_undo(void *ctx, uint64_t run_id, int from, int to)
{
	struct rec *r = ctx;
	r->undos++;
	r->undo_from = from;
	r->undo_to = to;
	r->cancelled += clipq_cancel(&r->q, run_id, from);
}

static void setup(struct tracker *t, struct rec *r)
{
	struct tracker_callbacks cb = {r, rec_run_started, rec_run_ended, rec_gold, rec_undo};
	memset(r, 0, sizeof(*r));
	clipq_init(&r->q);
	r->lead = 2.0;
	r->tail = 2.0;
	tracker_init(t, &cb);
}

static void teardown(struct tracker *t, struct rec *r, struct fake_ls *f)
{
	CHECK_I64(f->bad_commands, 0);
	tracker_free(t);
	clipq_free(&r->q);
}

#define POLL(ms) CHECK_I64(tracker_poll(&t, MS(ms), fake_query, &f), TRACKER_OK)

/* bests per segment 10, 15, 20, 20 */
static const char *bs4[] = {"00:00:10", "00:00:25", "00:00:45", "00:01:05"};

static void test_tracker_golds(void)
{
	struct tracker t;
	struct rec r;
	struct fake_ls f;

	setup(&t, &r);
	fl_init(&f, 4, bs4);
	POLL(0);
	CHECK_I64(r.started, 0);
	fl_start(&f);
	POLL(1000);
	CHECK_I64(r.started, 1);
	CHECK(r.run.start_observed);
	CHECK_I64(r.run.split_count, 4);
	CHECK_I64(r.run.first_index, 0);
	POLL(1025); /* nothing happens */

	fl_split(&f, 9.5); /* 9.5 < 10: gold on the first split */
	POLL(10500);
	CHECK_I64(r.num_golds, 1);
	CHECK_I64(r.golds[0].split, 0);
	CHECK_STR(r.golds[0].name, "Level 1");
	CHECK_I64(r.golds[0].seg, SEC(9.5));
	CHECK(r.golds[0].has_best);
	CHECK_I64(r.golds[0].best, SEC(10));
	CHECK(r.golds[0].window_known);
	CHECK_I64(r.golds[0].start, MS(1000));
	CHECK_I64(r.golds[0].end, MS(10500));

	fl_split(&f, 24.5); /* 15.0 == 15: a tie is not a gold */
	POLL(26000);
	CHECK_I64(r.num_golds, 1);

	fl_split(&f, 44.4); /* 19.9 < 20 */
	POLL(46000);
	CHECK_I64(r.num_golds, 2);
	CHECK_I64(r.golds[1].split, 2);
	CHECK_STR(r.golds[1].name, "Boss: 3");
	CHECK_I64(r.golds[1].seg, SEC(19.9));
	CHECK_I64(r.golds[1].best, SEC(20));
	CHECK_I64(r.golds[1].start, MS(26000));
	CHECK_I64(r.golds[1].end, MS(46000));

	fl_split(&f, 64.5); /* 20.1 > 20, final split, Ended */
	POLL(66000);
	CHECK_I64(r.num_golds, 2);
	CHECK_STR(f.phase, "Ended");
	POLL(67000);
	CHECK_I64(r.ended, 0);

	/* both clips queued with lead and tail */
	CHECK_I64(r.q.num, 2);
	CHECK_I64(r.q.items[0].start_ns, MS(-1000));
	CHECK_I64(r.q.items[0].end_ns, MS(12500));
	CHECK_I64(r.q.items[1].start_ns, MS(24000));
	CHECK_I64(r.q.items[1].end_ns, MS(48000));

	fl_reset(&f);
	POLL(70000);
	CHECK_I64(r.ended, 1);
	CHECK_I64(r.ended_lost, 0);
	CHECK_I64(r.q.num, 2); /* reset keeps queued clips */
	teardown(&t, &r, &f);
}

static void test_tracker_final_split_gold(void)
{
	static const char *bs[] = {"00:00:10", "00:00:20"};
	struct tracker t;
	struct rec r;
	struct fake_ls f;

	setup(&t, &r);
	fl_init(&f, 2, bs);
	POLL(0);
	fl_start(&f);
	POLL(100);
	fl_split(&f, 10.5);
	POLL(10600);
	fl_split(&f, 19.0); /* 8.5 < 10 on the final split */
	POLL(19100);
	CHECK_STR(f.phase, "Ended");
	CHECK_I64(r.num_golds, 1);
	CHECK_I64(r.golds[0].split, 1);
	CHECK_STR(r.golds[0].name, "End");
	CHECK_I64(r.golds[0].seg, SEC(8.5));
	CHECK_I64(r.golds[0].start, MS(10600));
	CHECK_I64(r.golds[0].end, MS(19100));

	/* undo from Ended cancels the final split's clip */
	fl_undo(&f);
	POLL(20000);
	CHECK_I64(r.undos, 1);
	CHECK_I64(r.undo_from, 1);
	CHECK_I64(r.undo_to, 2);
	CHECK_I64(r.cancelled, 1);
	CHECK_I64(r.q.num, 0);

	/* split again, gold again, then reset: the clip stays */
	fl_split(&f, 19.5);
	POLL(21000);
	CHECK_I64(r.num_golds, 2);
	CHECK_I64(r.golds[1].start, MS(10600)); /* segment start unchanged by the undo */
	CHECK_I64(r.golds[1].end, MS(21000));
	fl_reset(&f);
	POLL(22000);
	CHECK_I64(r.ended, 1);
	CHECK_I64(r.q.num, 1);
	teardown(&t, &r, &f);
}

static void test_tracker_unknown_best(void)
{
	struct tracker t;
	struct rec r;
	struct fake_ls f;

	/* first run ever: no best segments, every split counts */
	setup(&t, &r);
	fl_init(&f, 3, NULL);
	POLL(0);
	fl_start(&f);
	POLL(100);
	fl_split(&f, 5);
	POLL(5100);
	fl_split(&f, 12);
	POLL(12100);
	fl_split(&f, 20);
	POLL(20100);
	CHECK_I64(r.num_golds, 3);
	CHECK(!r.golds[0].has_best);
	CHECK_I64(r.golds[1].seg, SEC(7));
	CHECK(!r.golds[2].has_best);
	teardown(&t, &r, &f);

	/* same with clip_unknown_best off */
	setup(&t, &r);
	t.clip_unknown_best = false;
	fl_init(&f, 3, NULL);
	POLL(0);
	fl_start(&f);
	POLL(100);
	fl_split(&f, 5);
	POLL(5100);
	fl_split(&f, 12);
	POLL(12100);
	CHECK_I64(r.num_golds, 0);
	teardown(&t, &r, &f);

	/* only split 2 lacks a best: it counts, split 3 cannot be compared */
	{
		static const char *bs[] = {"00:00:10", "-", "00:00:30"};
		setup(&t, &r);
		fl_init(&f, 3, bs);
		POLL(0);
		fl_start(&f);
		POLL(100);
		fl_split(&f, 11); /* 11 > 10 */
		POLL(11100);
		fl_split(&f, 20); /* no best: gold */
		POLL(20100);
		fl_split(&f, 21); /* 1 s, but the previous best total is unknown */
		POLL(21100);
		CHECK_I64(r.num_golds, 1);
		CHECK_I64(r.golds[0].split, 1);
		CHECK(!r.golds[0].has_best);
		teardown(&t, &r, &f);
	}
}

static void test_tracker_skip(void)
{
	struct tracker t;
	struct rec r;
	struct fake_ls f;

	setup(&t, &r);
	fl_init(&f, 4, bs4);
	POLL(0);
	fl_start(&f);
	POLL(100);
	fl_split(&f, 11); /* not gold */
	POLL(11100);
	fl_split(&f, -1); /* skip split 2 */
	POLL(20000);
	fl_split(&f, 30); /* would be quick, but split 2's time is unknown */
	POLL(30100);
	CHECK_I64(r.num_golds, 0);
	fl_split(&f, 45); /* 15 < 20: normal again */
	POLL(45100);
	CHECK_I64(r.num_golds, 1);
	CHECK_I64(r.golds[0].split, 3);
	CHECK_I64(r.golds[0].seg, SEC(15));
	CHECK_I64(r.golds[0].best, SEC(20));
	CHECK_I64(r.golds[0].start, MS(30100));
	teardown(&t, &r, &f);
}

static void test_tracker_jump(void)
{
	static const char *bs[] = {"00:00:10", "00:00:20", "00:00:30", "00:00:40", "00:00:50"};
	struct tracker t;
	struct rec r;
	struct fake_ls f;

	setup(&t, &r);
	fl_init(&f, 5, bs);
	POLL(0);
	fl_start(&f);
	POLL(100);
	fl_split(&f, 9); /* gold */
	POLL(9100);
	fl_split(&f, 18); /* 9 < 10 ... */
	fl_split(&f, 27); /* ... and 9 < 10, both within one poll */
	POLL(27100);
	CHECK_I64(r.num_golds, 1);
	fl_split(&f, 36); /* 9 < 10, but split 3's Best Segments total was never seen */
	POLL(36100);
	CHECK_I64(r.num_golds, 1);
	fl_split(&f, 45); /* 9 < 10 and everything is known again */
	POLL(45100);
	CHECK_I64(r.num_golds, 2);
	CHECK_I64(r.golds[1].split, 4);
	CHECK_I64(r.golds[1].start, MS(36100));
	teardown(&t, &r, &f);
}

static void test_tracker_undo(void)
{
	struct tracker t;
	struct rec r;
	struct fake_ls f;

	setup(&t, &r);
	fl_init(&f, 4, bs4);
	POLL(0);
	fl_start(&f);
	POLL(100);
	fl_split(&f, 9); /* gold, stays */
	POLL(9100);
	fl_split(&f, 23); /* 14 < 15: gold, will be undone */
	POLL(23100);
	CHECK_I64(r.num_golds, 2);
	CHECK_I64(r.q.num, 2);

	fl_undo(&f);
	POLL(24000);
	CHECK_I64(r.undos, 1);
	CHECK_I64(r.undo_from, 1);
	CHECK_I64(r.cancelled, 1);
	CHECK_I64(r.q.num, 1);
	CHECK_I64(r.q.items[0].split_index, 0);

	fl_split(&f, 25); /* 16 > 15 */
	POLL(25100);
	CHECK_I64(r.num_golds, 2);
	fl_split(&f, 44); /* 19 < 20: segment start is the redone split */
	POLL(44100);
	CHECK_I64(r.num_golds, 3);
	CHECK_I64(r.golds[2].start, MS(25100));

	/* undo two splits at once */
	fl_undo(&f);
	fl_undo(&f);
	POLL(45000);
	CHECK_I64(r.undos, 2);
	CHECK_I64(r.undo_from, 1);
	CHECK_I64(r.undo_to, 3);
	CHECK_I64(r.q.num, 1);
	teardown(&t, &r, &f);
}

static void test_tracker_reset_keeps_clips(void)
{
	struct tracker t;
	struct rec r;
	struct fake_ls f;

	setup(&t, &r);
	fl_init(&f, 4, bs4);
	POLL(0);
	fl_start(&f);
	POLL(100);
	fl_split(&f, 9); /* gold in run 1 */
	POLL(9100);
	fl_reset(&f);
	POLL(9500);
	CHECK_I64(r.ended, 1);
	CHECK_I64(r.q.num, 1);

	fl_start(&f);
	POLL(10000);
	CHECK_I64(r.started, 2);
	CHECK_I64(r.run.run_id, 2);
	fl_split(&f, 8); /* gold in run 2 */
	POLL(18000);
	CHECK_I64(r.q.num, 2);
	fl_undo(&f); /* cancels run 2's clip only */
	POLL(18500);
	CHECK_I64(r.q.num, 1);
	CHECK_I64(r.q.items[0].run_id, 1);
	teardown(&t, &r, &f);
}

static void test_tracker_mid_run(void)
{
	struct tracker t;
	struct rec r;
	struct fake_ls f;

	/* connected while the timer runs at split 3 */
	setup(&t, &r);
	fl_init(&f, 4, bs4);
	fl_start(&f);
	fl_split(&f, 10);
	fl_split(&f, 25);
	POLL(1000);
	CHECK_I64(r.started, 1);
	CHECK(!r.run.start_observed);
	CHECK_I64(r.run.first_index, 2);
	fl_split(&f, 40); /* 15 < 20, but split 2's time was never seen */
	POLL(2000);
	CHECK_I64(r.num_golds, 0);
	fl_split(&f, 55); /* 15 < 20 */
	POLL(3000);
	CHECK_I64(r.num_golds, 1);
	CHECK(r.golds[0].window_known);
	CHECK_I64(r.golds[0].start, MS(2000));
	teardown(&t, &r, &f);

	/* connected during the first segment: gold, but its start was not seen */
	setup(&t, &r);
	fl_init(&f, 4, bs4);
	fl_start(&f);
	POLL(1000);
	fl_split(&f, 9);
	POLL(2000);
	CHECK_I64(r.num_golds, 1);
	CHECK(!r.golds[0].window_known);
	CHECK_I64(r.q.num, 0);
	teardown(&t, &r, &f);
}

static void test_tracker_connection_lost(void)
{
	struct tracker t;
	struct rec r;
	struct fake_ls f;

	setup(&t, &r);
	fl_init(&f, 4, bs4);
	POLL(0);
	fl_start(&f);
	POLL(100);
	fl_split(&f, 9);
	POLL(9100);
	CHECK_I64(r.q.num, 1);

	f.fail = true;
	CHECK_I64(tracker_poll(&t, MS(9200), fake_query, &f), TRACKER_IO_ERROR);
	tracker_connection_lost(&t);
	CHECK_I64(r.ended, 1);
	CHECK_I64(r.ended_lost, 1);
	CHECK_I64(r.q.num, 1);

	/* reconnected: the run counts as one whose start was not seen */
	f.fail = false;
	fl_split(&f, 23); /* would be gold, but split 1's time belongs to the lost run */
	POLL(12000);
	CHECK_I64(r.started, 2);
	CHECK(!r.run.start_observed);
	fl_split(&f, 42); /* 19 < 20, but split 2's time was not seen either */
	POLL(42100);
	CHECK_I64(r.num_golds, 1);
	fl_split(&f, 61); /* 19 < 20, previous split seen after the reconnect */
	POLL(61100);
	CHECK_I64(r.num_golds, 2);
	CHECK_I64(r.golds[1].split, 3);
	CHECK(r.golds[1].window_known);
	CHECK_I64(r.golds[1].start, MS(42100));
	teardown(&t, &r, &f);
}

static void test_tracker_pause_and_protocol(void)
{
	struct tracker t;
	struct rec r;
	struct fake_ls f;

	setup(&t, &r);
	fl_init(&f, 4, bs4);
	POLL(0);
	fl_start(&f);
	POLL(100);
	f.phase = "Paused";
	POLL(5000);
	CHECK_I64(r.ended, 0);
	f.phase = "Running";
	fl_split(&f, 9);
	POLL(20000);
	CHECK_I64(r.num_golds, 1);
	CHECK_I64(r.golds[0].end - r.golds[0].start, MS(19900)); /* the pause is inside */

	/* Running with index -1 (a reset between the two queries) is not a run */
	f.phase = "Running";
	f.index = -1;
	POLL(21000);
	CHECK_I64(r.ended, 1);

	f.phase = "Bogus";
	CHECK_I64(tracker_poll(&t, MS(22000), fake_query, &f), TRACKER_PROTOCOL_ERROR);
	teardown(&t, &r, &f);

	{
		enum ls_phase p;
		CHECK(ls_parse_phase("NotRunning", &p) && p == LS_PHASE_NOT_RUNNING);
		CHECK(ls_parse_phase("Running", &p) && p == LS_PHASE_RUNNING);
		CHECK(ls_parse_phase("Ended", &p) && p == LS_PHASE_ENDED);
		CHECK(ls_parse_phase("Paused", &p) && p == LS_PHASE_PAUSED);
		CHECK(!ls_parse_phase("running", &p));
	}
}

/* ------------------------------------------------------------------------ */
/* command lines and processes                                              */

static void test_cmdline_roundtrip(void)
{
	const char *args[] = {
		"C:/Program Files/ffmpeg/bin/ffmpeg.exe",
		"-i",
		"C:\\a b\\c.mkv",
		"",
		"a\"b",
		"trail\\",
		"trail space\\",
		"back\\\\\"quote",
		"\\\\server\\share\\x y",
		("Gr\xC3\xB6\xC3\x9F" "e \xE6\x97\xA5\xE6\x9C\xAC.mkv"),
		"-sseof",
		"-12.300",
	};
	size_t n = sizeof(args) / sizeof(args[0]);
	char *cmd = cmdline_build(args, n);
	wchar_t *wcmd = cmd ? utf8_to_wide(cmd) : NULL;
	int argc = 0;
	wchar_t **argv = wcmd ? CommandLineToArgvW(wcmd, &argc) : NULL;

	CHECK(argv != NULL);
	CHECK_I64(argc, n);
	for (int i = 0; argv && i < argc && (size_t)i < n; i++) {
		char *u = wide_to_utf8(argv[i]);
		CHECK(u != NULL);
		if (u) {
			CHECK_STR(u, args[i]);
			free(u);
		}
	}
	if (argv)
		LocalFree(argv);
	free(wcmd);
	free(cmd);

	{
		const char *simple[] = {"ffmpeg", "-c", "copy"};
		cmd = cmdline_build(simple, 3);
		CHECK_STR(cmd, "ffmpeg -c copy");
		free(cmd);
	}
}

static void test_ffmpeg_args(void)
{
	char nums[2][32];
	const char *argv[24];
	size_t argc = ffmpeg_clip_args("ffmpeg", "in.mkv", 12300, 5005, "out.mkv", nums, argv);
	char *cmd = cmdline_build(argv, argc);

	CHECK_STR(cmd, "ffmpeg -hide_banner -nostdin -loglevel error -sseof -12.300 -i in.mkv -t 5.005 -map 0 "
		       "-c copy -avoid_negative_ts make_zero -n out.mkv");
	CHECK(argv[argc] == NULL);
	free(cmd);
}

static char self_path[MAX_PATH * 4];

static void test_proc_run(void)
{
	struct proc_result *res = malloc(sizeof(*res));
	HANDLE cancel;

	{
		const char *argv[] = {self_path, "--child-exit", "7", "Gr\xC3\xB6\xC3\x9F" "e \"q\""};
		proc_run(argv, 4, 10000, NULL, res);
		CHECK_I64(res->status, PROC_EXITED);
		CHECK_I64(res->exit_code, 7);
		CHECK(strstr(res->output, "child stderr: Gr\xC3\xB6\xC3\x9F" "e \"q\"") != NULL);
		CHECK(strstr(res->output, "child stdout") != NULL);
	}
	{
		const char *argv[] = {self_path, "--child-exit", "0", "x"};
		proc_run(argv, 4, 10000, NULL, res);
		CHECK_I64(res->status, PROC_EXITED);
		CHECK_I64(res->exit_code, 0);
	}
	{
		const char *argv[] = {self_path, "--child-sleep", "10000"};
		ULONGLONG t0 = GetTickCount64();
		proc_run(argv, 3, 300, NULL, res);
		CHECK_I64(res->status, PROC_TIMEOUT);
		CHECK(GetTickCount64() - t0 < 5000);
	}
	{
		const char *argv[] = {self_path, "--child-sleep", "10000"};
		cancel = CreateEventW(NULL, TRUE, TRUE, NULL);
		proc_run(argv, 3, 10000, cancel, res);
		CHECK_I64(res->status, PROC_CANCELLED);
		CloseHandle(cancel);
	}
	{
		const char *argv[] = {"gold-clips-no-such-program-xyz", "-version"};
		proc_run(argv, 2, 10000, NULL, res);
		CHECK_I64(res->status, PROC_START_FAILED);
		CHECK_I64(res->start_error, ERROR_FILE_NOT_FOUND);
	}
	{
		/* lots of output must not block the child */
		const char *argv[] = {self_path, "--child-spam"};
		proc_run(argv, 2, 10000, NULL, res);
		CHECK_I64(res->status, PROC_EXITED);
		CHECK_I64(strlen(res->output), PROC_OUTPUT_MAX - 1);
	}
	free(res);
}

static int child_main(int argc, wchar_t **argv)
{
	if (argc >= 4 && wcscmp(argv[1], L"--child-exit") == 0) {
		char *text = wide_to_utf8(argv[3]);
		fprintf(stdout, "child stdout\n");
		fprintf(stderr, "child stderr: %s\n", text ? text : "?");
		free(text);
		return _wtoi(argv[2]);
	}
	if (argc >= 3 && wcscmp(argv[1], L"--child-sleep") == 0) {
		Sleep((DWORD)_wtoi(argv[2]));
		return 0;
	}
	if (argc >= 2 && wcscmp(argv[1], L"--child-spam") == 0) {
		for (int i = 0; i < 20000; i++)
			fprintf(stderr, "line %d of output that nobody reads\n", i);
		return 0;
	}
	return -1;
}

/* ------------------------------------------------------------------------ */

#define RUN(fn)                    \
	do {                       \
		current_case = #fn; \
		cases_run++;       \
		fn();              \
	} while (0)

int main(void)
{
	int wargc = 0;
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);

	if (wargv && wargc > 1)
		return child_main(wargc, wargv);
	GetModuleFileNameA(NULL, self_path, sizeof(self_path));

	gc_log_set_sink(getenv("GOLD_CLIPS_TEST_VERBOSE") ? NULL : quiet_sink);

	RUN(test_lstime_parse);
	RUN(test_lstime_format);
	RUN(test_sanitize);
	RUN(test_paths);
	RUN(test_unique_path);
	RUN(test_cut_params);
	RUN(test_clipq);
	RUN(test_tracker_golds);
	RUN(test_tracker_final_split_gold);
	RUN(test_tracker_unknown_best);
	RUN(test_tracker_skip);
	RUN(test_tracker_jump);
	RUN(test_tracker_undo);
	RUN(test_tracker_reset_keeps_clips);
	RUN(test_tracker_mid_run);
	RUN(test_tracker_connection_lost);
	RUN(test_tracker_pause_and_protocol);
	RUN(test_cmdline_roundtrip);
	RUN(test_ffmpeg_args);
	RUN(test_proc_run);

	printf("unit tests: %d cases, %d checks, %d failed\n", cases_run, checks_run, checks_failed);
	return checks_failed ? 1 : 0;
}
