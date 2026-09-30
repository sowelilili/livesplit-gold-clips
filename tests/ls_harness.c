/*
 * Runs the real poller (TCP client + gold tracker) against a LiveSplit
 * server for a fixed time and prints what it saw, one event per line with
 * tab-separated key=value fields:
 *
 *   ls_harness <port> <duration_ms> [host]
 *
 *   RUN_START  run= game= category= splits= first= observed=
 *   RUN_END    run= lost=
 *   GOLD       run= split= name= seg= best= window= wall_ms=
 *   UNDO       run= from= to= cancelled=
 *   QUEUED     run= split= name= seg=        (clips left at the end)
 *
 * Times: seg/best in LiveSplit ticks (best=- when there was none), wall_ms
 * is the segment's length on the plugin clock.
 */
#include "clips.h"
#include "gclog.h"
#include "poller.h"
#include "winutil.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *host = "127.0.0.1";
static int port;
static struct clipq queue;
static CRITICAL_SECTION lock;

static void out_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void out_line(const char *fmt, ...)
{
	va_list args;

	EnterCriticalSection(&lock);
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	fputc('\n', stdout);
	fflush(stdout);
	LeaveCriticalSection(&lock);
}

static int64_t now_ns(void *ctx)
{
	(void)ctx;
	return qpc_now_ns();
}

static void load_settings(void *ctx, struct poller_settings *s)
{
	(void)ctx;
	snprintf(s->host, sizeof(s->host), "%s", host);
	s->port = port;
	s->poll_ms = 25;
	s->clip_unknown_best = true;
}

static void on_run_started(void *ctx, const struct run_info *run)
{
	(void)ctx;
	out_line("RUN_START\trun=%llu\tgame=%s\tcategory=%s\tsplits=%d\tfirst=%d\tobserved=%d",
		 (unsigned long long)run->run_id, run->game, run->category, run->split_count, run->first_index,
		 run->start_observed ? 1 : 0);
}

static void on_run_ended(void *ctx, uint64_t run_id, bool lost)
{
	(void)ctx;
	out_line("RUN_END\trun=%llu\tlost=%d", (unsigned long long)run_id, lost ? 1 : 0);
}

static void on_gold(void *ctx, const struct gold_event *ev)
{
	char best[32];
	struct clip c;

	(void)ctx;
	if (ev->has_best)
		snprintf(best, sizeof(best), "%lld", (long long)ev->best_ticks);
	else
		strcpy(best, "-");
	out_line("GOLD\trun=%llu\tsplit=%d\tname=%s\tseg=%lld\tbest=%s\twindow=%d\twall_ms=%lld",
		 (unsigned long long)ev->run_id, ev->split_index, ev->split_name, (long long)ev->seg_ticks, best,
		 ev->window_known ? 1 : 0, (long long)((ev->seg_end_ns - ev->seg_start_ns) / 1000000));

	if (ev->window_known) {
		memset(&c, 0, sizeof(c));
		c.run_id = ev->run_id;
		c.split_index = ev->split_index;
		c.game = (char *)ev->game;
		c.category = (char *)ev->category;
		c.split_name = (char *)ev->split_name;
		c.seg_ticks = ev->seg_ticks;
		clip_window(ev->seg_start_ns, ev->seg_end_ns, 2.0, 2.0, &c.start_ns, &c.end_ns);
		EnterCriticalSection(&lock);
		clipq_add(&queue, &c);
		LeaveCriticalSection(&lock);
	}
}

static void on_undo(void *ctx, uint64_t run_id, int from, int to)
{
	size_t n;

	(void)ctx;
	EnterCriticalSection(&lock);
	n = clipq_cancel(&queue, run_id, from);
	LeaveCriticalSection(&lock);
	out_line("UNDO\trun=%llu\tfrom=%d\tto=%d\tcancelled=%zu", (unsigned long long)run_id, from, to, n);
}

int main(int argc, char **argv)
{
	struct poller poller;
	struct poller_hooks hooks;
	int duration_ms;

	if (argc < 3) {
		fprintf(stderr, "usage: ls_harness <port> <duration_ms> [host]\n");
		return 2;
	}
	port = atoi(argv[1]);
	duration_ms = atoi(argv[2]);
	if (argc > 3)
		host = argv[3];
	if (port == 16834) {
		/* tests must never talk to a real LiveSplit */
		fprintf(stderr, "refusing to use LiveSplit's default port 16834\n");
		return 2;
	}

	InitializeCriticalSection(&lock);
	clipq_init(&queue);

	memset(&hooks, 0, sizeof(hooks));
	hooks.now_ns = now_ns;
	hooks.load_settings = load_settings;
	hooks.events.run_started = on_run_started;
	hooks.events.run_ended = on_run_ended;
	hooks.events.gold = on_gold;
	hooks.events.undo = on_undo;
	if (!poller_start(&poller, &hooks))
		return 1;
	Sleep((DWORD)duration_ms);
	poller_stop(&poller);

	for (size_t i = 0; i < queue.num; i++)
		out_line("QUEUED\trun=%llu\tsplit=%d\tname=%s\tseg=%lld", (unsigned long long)queue.items[i].run_id,
			 queue.items[i].split_index, queue.items[i].split_name, (long long)queue.items[i].seg_ticks);
	clipq_free(&queue);
	DeleteCriticalSection(&lock);
	return 0;
}
