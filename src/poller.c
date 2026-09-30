#include "poller.h"

#include "gclog.h"

#include <windows.h>

#include <string.h>

#define NS_PER_MS 1000000LL

static void load_settings(struct poller *p)
{
	struct poller_settings s;

	memset(&s, 0, sizeof(s));
	p->hooks.load_settings(p->hooks.ctx, &s);
	if (s.poll_ms < 5)
		s.poll_ms = 5;
	if (s.poll_ms > 1000)
		s.poll_ms = 1000;
	p->settings = s;
	p->tracker.clip_unknown_best = s.clip_unknown_best;
}

static void on_run_started(void *ctx, const struct run_info *run)
{
	struct poller *p = ctx;

	load_settings(p); /* settings edits apply from the next run */
	if (p->hooks.events.run_started)
		p->hooks.events.run_started(p->hooks.events.ctx, run);
}

static void on_run_ended(void *ctx, uint64_t run_id, bool connection_lost)
{
	struct poller *p = ctx;
	if (p->hooks.events.run_ended)
		p->hooks.events.run_ended(p->hooks.events.ctx, run_id, connection_lost);
}

static void on_gold(void *ctx, const struct gold_event *ev)
{
	struct poller *p = ctx;
	if (p->hooks.events.gold)
		p->hooks.events.gold(p->hooks.events.ctx, ev);
}

static void on_undo(void *ctx, uint64_t run_id, int from_split, int to_split)
{
	struct poller *p = ctx;
	if (p->hooks.events.undo)
		p->hooks.events.undo(p->hooks.events.ctx, run_id, from_split, to_split);
}

static bool query(void *ctx, const char *cmd, char *out, size_t out_size)
{
	struct poller *p = ctx;
	return ls_query(&p->conn, cmd, out, out_size);
}

static DWORD WINAPI poller_main(LPVOID param)
{
	struct poller *p = param;
	bool failure_logged = false;
	int64_t next_attempt = 0;

	for (;;) {
		int64_t now = p->hooks.now_ns(p->hooks.ctx);

		if (p->hooks.tick)
			p->hooks.tick(p->hooks.ctx, now);

		if (!ls_connected(&p->conn) && now >= next_attempt) {
			load_settings(p);
			if (ls_connect(&p->conn, p->settings.host, p->settings.port, POLLER_CONNECT_TIMEOUT_MS)) {
				gc_log(GC_LOG_INFO, "connected to LiveSplit at %s:%d", p->settings.host,
				       p->settings.port);
				failure_logged = false;
			} else {
				if (!failure_logged)
					gc_log(GC_LOG_INFO, "cannot connect to LiveSplit at %s:%d: %s; retrying every %d s",
					       p->settings.host, p->settings.port, p->conn.error,
					       POLLER_RECONNECT_INTERVAL_MS / 1000);
				failure_logged = true;
				next_attempt = p->hooks.now_ns(p->hooks.ctx) + POLLER_RECONNECT_INTERVAL_MS * NS_PER_MS;
			}
		}

		if (ls_connected(&p->conn)) {
			int r;
			now = p->hooks.now_ns(p->hooks.ctx);
			r = tracker_poll(&p->tracker, now, query, p);
			if (r != TRACKER_OK) {
				gc_log(GC_LOG_INFO, "lost connection to LiveSplit: %s; reconnecting every %d s",
				       r == TRACKER_IO_ERROR ? p->conn.error : "unexpected response",
				       POLLER_RECONNECT_INTERVAL_MS / 1000);
				ls_close(&p->conn);
				tracker_connection_lost(&p->tracker);
				failure_logged = true; /* quiet until connected again */
				next_attempt = now + POLLER_RECONNECT_INTERVAL_MS * NS_PER_MS;
			}
		}

		if (WaitForSingleObject(p->stop, (DWORD)p->settings.poll_ms) == WAIT_OBJECT_0)
			break;
	}

	ls_close(&p->conn);
	return 0;
}

bool poller_start(struct poller *p, const struct poller_hooks *hooks)
{
	struct tracker_callbacks cb;

	memset(p, 0, sizeof(*p));
	p->hooks = *hooks;
	memset(&cb, 0, sizeof(cb));
	cb.ctx = p;
	cb.run_started = on_run_started;
	cb.run_ended = on_run_ended;
	cb.gold = on_gold;
	cb.undo = on_undo;
	tracker_init(&p->tracker, &cb);
	ls_conn_init(&p->conn, POLLER_RESPONSE_TIMEOUT_MS);
	p->settings.poll_ms = 25;

	if (!ls_net_init()) {
		gc_log(GC_LOG_ERROR, "Winsock initialisation failed");
		return false;
	}
	p->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (p->stop)
		p->thread = CreateThread(NULL, 0, poller_main, p, 0, NULL);
	if (!p->thread) {
		gc_log(GC_LOG_ERROR, "could not start the LiveSplit polling thread");
		if (p->stop)
			CloseHandle(p->stop);
		p->stop = NULL;
		ls_net_cleanup();
		tracker_free(&p->tracker);
		return false;
	}
	return true;
}

void poller_stop(struct poller *p)
{
	if (!p->thread)
		return;
	SetEvent(p->stop);
	WaitForSingleObject(p->thread, INFINITE);
	CloseHandle(p->thread);
	CloseHandle(p->stop);
	p->thread = NULL;
	p->stop = NULL;
	tracker_free(&p->tracker);
	ls_net_cleanup();
}
