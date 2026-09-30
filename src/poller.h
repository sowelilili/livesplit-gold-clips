/*
 * The LiveSplit polling thread: keeps one connection to LiveSplit, feeds the
 * gold tracker every poll_ms, reconnects every 2 s after a failure and calls
 * a tick hook every loop so the host can schedule its own work (replay saves).
 */
#pragma once

#include "lsclient.h"
#include "tracker.h"

#include <stdbool.h>
#include <stdint.h>

#define POLLER_CONNECT_TIMEOUT_MS 1000
#define POLLER_RESPONSE_TIMEOUT_MS 1000
#define POLLER_RECONNECT_INTERVAL_MS 2000

struct poller_settings {
	char host[256];
	int port;
	int poll_ms;
	bool clip_unknown_best;
};

struct poller_hooks {
	void *ctx;
	int64_t (*now_ns)(void *ctx);
	/* Called before every connection attempt and at every run start. */
	void (*load_settings)(void *ctx, struct poller_settings *s);
	/* Called on every loop iteration (every poll_ms), connected or not. */
	void (*tick)(void *ctx, int64_t now_ns);
	/* Tracker events, all on the poller thread; the ctx inside is used. */
	struct tracker_callbacks events;
};

struct poller {
	struct poller_hooks hooks;
	struct poller_settings settings;
	struct tracker tracker;
	struct ls_conn conn;
	void *thread;
	void *stop;
};

bool poller_start(struct poller *p, const struct poller_hooks *hooks);
void poller_stop(struct poller *p);
