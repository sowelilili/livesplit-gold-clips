/*
 * OBS glue: settings, the replay buffer save cycle, the Tools menu entry and
 * module lifetime. Threads: the poller thread (LiveSplit + save scheduling),
 * the extraction worker (ffmpeg) and OBS's UI thread (frontend calls and
 * events). Shared state lives in `g` under g.lock.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <windows.h>
#include <shellapi.h>

#include "clips.h"
#include "extract.h"
#include "gclog.h"
#include "lstime.h"
#include "poller.h"
#include "tracker.h"
#include "winutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLUGIN_VERSION "1.0.0"
#define SECTION "General"
#define NS_PER_SECOND 1000000000LL
#define SAVE_TIMEOUT_NS (15 * NS_PER_SECOND)
#define EXIT_GRACE_MS 10000

OBS_DECLARE_MODULE()

const char *obs_module_name(void)
{
	return "LiveSplit Gold Clips";
}

const char *obs_module_description(void)
{
	return "Saves a clip of every gold split in LiveSplit from the replay buffer.";
}

struct gc_settings {
	char host[256];
	int port;
	int poll_ms;
	double lead_seconds;
	double tail_seconds;
	double save_margin_seconds;
	char folder[260];
	char ffmpeg[1024];
	bool start_replay_buffer;
	bool keep_full_replay;
	bool clip_unknown_best;
};

static struct {
	CRITICAL_SECTION lock;     /* everything below except cfg_lock's file access */
	CRITICAL_SECTION cfg_lock; /* serialises reading/writing config.ini */
	bool locks_ready;

	struct gc_settings settings;
	bool settings_logged;

	struct clipq queue;
	bool save_outstanding;
	uint64_t save_seq;
	int64_t save_queued_ns;
	int64_t save_t_req_ns; /* 0 until the UI thread has asked OBS */
	double save_buffer_seconds;
	int64_t rb_started_ns; /* when the replay buffer last started, 0 if unknown */

	struct poller poller;
	struct extract_worker worker;
	bool threads_running;
	bool shutting_down;
	bool callback_registered;
} g;

/* ------------------------------------------------------------------------ */
/* settings                                                                 */

static const struct {
	const char *key;
	const char *def;
	const char *comment;
} setting_keys[] = {
	{"host", "127.0.0.1", "LiveSplit's TCP server (LiveSplit: right-click > Control > Start TCP Server)"},
	{"port", "16834", NULL},
	{"poll_ms", "25", "How often to ask LiveSplit for the current split, in milliseconds"},
	{"lead_seconds", "2.0", "Extra video before the segment starts and after it ends, in seconds"},
	{"tail_seconds", "2.0", NULL},
	{"save_margin_seconds", "1.0", "Wait this long after a clip's end before saving the replay buffer"},
	{"folder", "Golds", "Clips go to <recording folder>/<folder>/<game>/<category>/"},
	{"ffmpeg", "ffmpeg", "ffmpeg program: a name on PATH or a full path written with forward slashes"},
	{"start_replay_buffer", "true", "Start the replay buffer when a run starts (true/false)"},
	{"keep_full_replay", "false", "Keep the full replay file after the clips are cut (true/false)"},
	{"clip_unknown_best", "true", "Count splits without a best segment yet as golds (true/false)"},
};

#define NUM_SETTINGS (sizeof(setting_keys) / sizeof(setting_keys[0]))

static void default_settings(struct gc_settings *s)
{
	memset(s, 0, sizeof(*s));
	strcpy(s->host, "127.0.0.1");
	s->port = 16834;
	s->poll_ms = 25;
	s->lead_seconds = 2.0;
	s->tail_seconds = 2.0;
	s->save_margin_seconds = 1.0;
	strcpy(s->folder, "Golds");
	strcpy(s->ffmpeg, "ffmpeg");
	s->start_replay_buffer = true;
	s->keep_full_replay = false;
	s->clip_unknown_best = true;
}

static void copy_trimmed(char *dst, size_t size, const char *src, const char *fallback)
{
	const char *s = src ? src : fallback;
	size_t n;

	while (*s == ' ' || *s == '\t')
		s++;
	n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
		n--;
	if (n == 0 && src) {
		copy_trimmed(dst, size, NULL, fallback); /* empty value: use the default */
		return;
	}
	if (n >= size)
		n = size - 1;
	memcpy(dst, s, n);
	dst[n] = '\0';
}

static double clamp_double(double v, double lo, double hi, double def)
{
	if (!(v == v)) /* NaN */
		return def;
	return v < lo ? lo : v > hi ? hi : v;
}

static bool write_template(const char *path)
{
	FILE *f = os_fopen(path, "wb");

	if (!f)
		return false;
	fprintf(f, "# LiveSplit Gold Clips settings. Edits apply at the next LiveSplit run start.\n"
		   "# Write key=value with no spaces around '='. Lines starting with # are comments.\n"
		   "[" SECTION "]\n");
	for (size_t i = 0; i < NUM_SETTINGS; i++) {
		if (setting_keys[i].comment)
			fprintf(f, "\n# %s\n", setting_keys[i].comment);
		fprintf(f, "%s=%s\n", setting_keys[i].key, setting_keys[i].def);
	}
	fclose(f);
	return true;
}

static char *config_file_path(void)
{
	return obs_module_config_path("config.ini");
}

/* Reads config.ini, creating it with the defaults (or adding missing keys)
 * first. Falls back to the defaults for anything unreadable. */
static void read_settings(struct gc_settings *s)
{
	char *path, *dir;
	config_t *cfg = NULL;
	bool any = false, missing = false;

	default_settings(s);
	EnterCriticalSection(&g.cfg_lock);

	path = config_file_path();
	dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	if (!path) {
		LeaveCriticalSection(&g.cfg_lock);
		return;
	}
	if (config_open(&cfg, path, CONFIG_OPEN_ALWAYS) != CONFIG_SUCCESS) {
		gc_log(GC_LOG_WARNING, "cannot open %s; using default settings", path);
		goto out;
	}

	for (size_t i = 0; i < NUM_SETTINGS; i++) {
		if (config_has_user_value(cfg, SECTION, setting_keys[i].key))
			any = true;
		else
			missing = true;
	}
	if (!any) {
		config_close(cfg);
		cfg = NULL;
		if (!write_template(path) || config_open(&cfg, path, CONFIG_OPEN_ALWAYS) != CONFIG_SUCCESS) {
			gc_log(GC_LOG_WARNING, "cannot write %s; using default settings", path);
			goto out;
		}
		gc_log(GC_LOG_INFO, "wrote default settings to %s", path);
	} else if (missing) {
		for (size_t i = 0; i < NUM_SETTINGS; i++)
			if (!config_has_user_value(cfg, SECTION, setting_keys[i].key))
				config_set_string(cfg, SECTION, setting_keys[i].key, setting_keys[i].def);
		if (config_save_safe(cfg, "tmp", NULL) != CONFIG_SUCCESS)
			gc_log(GC_LOG_WARNING, "cannot add the missing settings to %s", path);
	}

	copy_trimmed(s->host, sizeof(s->host), config_get_string(cfg, SECTION, "host"), "127.0.0.1");
	s->port = (int)config_get_int(cfg, SECTION, "port");
	if (s->port < 1 || s->port > 65535)
		s->port = 16834;
	s->poll_ms = (int)config_get_int(cfg, SECTION, "poll_ms");
	if (s->poll_ms < 5 || s->poll_ms > 1000)
		s->poll_ms = s->poll_ms < 5 ? 5 : 1000;
	s->lead_seconds = clamp_double(config_get_double(cfg, SECTION, "lead_seconds"), 0, 600, 2.0);
	s->tail_seconds = clamp_double(config_get_double(cfg, SECTION, "tail_seconds"), 0, 600, 2.0);
	s->save_margin_seconds = clamp_double(config_get_double(cfg, SECTION, "save_margin_seconds"), 0, 60, 1.0);
	copy_trimmed(s->folder, sizeof(s->folder), config_get_string(cfg, SECTION, "folder"), "");
	copy_trimmed(s->ffmpeg, sizeof(s->ffmpeg), config_get_string(cfg, SECTION, "ffmpeg"), "ffmpeg");
	s->start_replay_buffer = config_get_bool(cfg, SECTION, "start_replay_buffer");
	s->keep_full_replay = config_get_bool(cfg, SECTION, "keep_full_replay");
	s->clip_unknown_best = config_get_bool(cfg, SECTION, "clip_unknown_best");

out:
	if (cfg)
		config_close(cfg);
	bfree(path);
	LeaveCriticalSection(&g.cfg_lock);
}

static void log_settings(const char *what, const struct gc_settings *s)
{
	gc_log(GC_LOG_INFO,
	       "%s: LiveSplit %s:%d, poll %d ms, lead %.1f s, tail %.1f s, save margin %.1f s, "
	       "folder \"%s\", ffmpeg \"%s\", start_replay_buffer %s, keep_full_replay %s, clip_unknown_best %s",
	       what, s->host, s->port, s->poll_ms, s->lead_seconds, s->tail_seconds, s->save_margin_seconds,
	       s->folder, s->ffmpeg, s->start_replay_buffer ? "true" : "false",
	       s->keep_full_replay ? "true" : "false", s->clip_unknown_best ? "true" : "false");
}

static void reload_settings(void)
{
	struct gc_settings s;
	bool changed;

	read_settings(&s);
	EnterCriticalSection(&g.lock);
	changed = memcmp(&s, &g.settings, sizeof(s)) != 0;
	g.settings = s;
	LeaveCriticalSection(&g.lock);
	if (changed || !g.settings_logged)
		log_settings(g.settings_logged ? "settings changed" : "settings", &s);
	g.settings_logged = true;
}

/* ------------------------------------------------------------------------ */
/* replay buffer (UI thread)                                                */

/* Replay buffer length from the current profile (Settings > Output). */
static int replay_buffer_seconds(void)
{
	config_t *cfg = obs_frontend_get_profile_config();
	const char *mode;
	bool advanced;

	if (!cfg)
		return 0;
	mode = config_get_string(cfg, "Output", "Mode");
	advanced = mode && _stricmp(mode, "Advanced") == 0;
	return (int)config_get_int(cfg, advanced ? "AdvOut" : "SimpleOutput", "RecRBTime");
}

static bool is_shutting_down(void)
{
	bool r;
	EnterCriticalSection(&g.lock);
	r = g.shutting_down;
	LeaveCriticalSection(&g.lock);
	return r;
}

static void ui_run_started(void *param)
{
	obs_output_t *rb;
	bool start;
	int seconds;

	(void)param;
	if (is_shutting_down())
		return;

	rb = obs_frontend_get_replay_buffer_output();
	if (!rb) {
		gc_log(GC_LOG_WARNING,
		       "the replay buffer is not enabled (Settings > Output > Replay Buffer); golds cannot be clipped");
		return;
	}
	obs_output_release(rb);

	EnterCriticalSection(&g.lock);
	start = g.settings.start_replay_buffer;
	LeaveCriticalSection(&g.lock);

	seconds = replay_buffer_seconds();
	if (obs_frontend_replay_buffer_active()) {
		gc_log(GC_LOG_INFO, "replay buffer is running (%d s)", seconds);
	} else if (start) {
		gc_log(GC_LOG_INFO, "starting the replay buffer (%d s)", seconds);
		obs_frontend_replay_buffer_start();
	} else {
		gc_log(GC_LOG_WARNING, "the replay buffer is not running; golds cannot be clipped until it is started");
	}
}

static void log_dropped(const char *why, struct clip *clips, size_t n)
{
	for (size_t i = 0; i < n; i++)
		gc_log(GC_LOG_WARNING, "dropped the clip for split %d \"%s\": %s", clips[i].split_index + 1,
		       clips[i].split_name, why);
	clip_free_array(clips, n);
}

static void ui_save_replay(void *param)
{
	uint64_t seq = (uint64_t)(uintptr_t)param;
	int configured_seconds;
	double buffer_seconds, uptime;
	bool limited_by_uptime = false;
	int64_t now;
	size_t count = 0;

	if (is_shutting_down())
		return;
	configured_seconds = replay_buffer_seconds();

	EnterCriticalSection(&g.lock);
	if (!g.save_outstanding || g.save_seq != seq || g.save_t_req_ns) {
		LeaveCriticalSection(&g.lock);
		return; /* timed out meanwhile */
	}
	now = (int64_t)os_gettime_ns();

	if (!obs_frontend_replay_buffer_active()) {
		struct clip *dropped;
		size_t n = clipq_take_ended_by(&g.queue, now, &dropped);
		g.save_outstanding = false;
		LeaveCriticalSection(&g.lock);
		gc_log(GC_LOG_WARNING, "the replay buffer is not active, so %zu clip(s) cannot be saved", n);
		log_dropped("replay buffer not active", dropped, n);
		return;
	}

	/* How far back the saved file will reach: the configured length, or
	 * less when the buffer started more recently than that (for example
	 * when this plugin started it at the run start). */
	buffer_seconds = configured_seconds;
	uptime = g.rb_started_ns ? (double)(now - g.rb_started_ns) / (double)NS_PER_SECOND : 0;
	if (uptime > 0 && (buffer_seconds <= 0 || uptime < buffer_seconds)) {
		buffer_seconds = uptime;
		limited_by_uptime = true;
	}

	for (size_t i = 0; i < g.queue.num; i++) {
		const struct clip *c = &g.queue.items[i];
		double needed = (double)(now - c->start_ns) / (double)NS_PER_SECOND;
		if (c->end_ns > now)
			continue;
		count++;
		if (buffer_seconds <= 0 || needed <= buffer_seconds)
			continue;
		if (limited_by_uptime)
			gc_log(GC_LOG_INFO,
			       "the replay buffer started only %.1f s ago, so the clip for split %d \"%s\" "
			       "starts late",
			       buffer_seconds, c->split_index + 1, c->split_name);
		else
			gc_log(GC_LOG_WARNING,
			       "the clip for split %d \"%s\" needs %.1f s of replay but the buffer holds %d s; "
			       "it will start late",
			       c->split_index + 1, c->split_name, needed, configured_seconds);
	}
	g.save_buffer_seconds = buffer_seconds;
	g.save_t_req_ns = (int64_t)os_gettime_ns();
	LeaveCriticalSection(&g.lock);

	gc_log(GC_LOG_INFO, "saving the replay buffer for %zu clip(s)", count);
	obs_frontend_replay_buffer_save();
}

static void on_replay_saved(void)
{
	char *path = obs_frontend_get_last_replay();
	struct extract_job *job;
	struct clip *clips = NULL;
	size_t n;

	EnterCriticalSection(&g.lock);
	if (!g.save_outstanding || !g.save_t_req_ns || !g.threads_running) {
		LeaveCriticalSection(&g.lock);
		bfree(path);
		return; /* a save someone else asked for */
	}
	n = clipq_take_ended_by(&g.queue, g.save_t_req_ns, &clips);
	g.save_outstanding = false;

	job = calloc(1, sizeof(*job));
	if (job) {
		job->t_req_ns = g.save_t_req_ns;
		job->buffer_seconds = g.save_buffer_seconds;
		job->keep_full_replay = g.settings.keep_full_replay;
		job->folder = _strdup(g.settings.folder);
		job->ffmpeg = _strdup(g.settings.ffmpeg);
		job->replay_path = _strdup(path ? path : "");
		job->clips = clips;
		job->num_clips = n;
	}
	LeaveCriticalSection(&g.lock);

	if (!job || !job->folder || !job->ffmpeg || !job->replay_path) {
		gc_log(GC_LOG_ERROR, "out of memory");
		if (job)
			extract_job_free(job);
		else
			clip_free_array(clips, n);
		bfree(path);
		return;
	}
	if (!path || !*path) {
		gc_log(GC_LOG_ERROR, "the replay buffer was saved but OBS did not report the file");
		extract_job_free(job);
		bfree(path);
		return;
	}
	bfree(path);

	gc_log(GC_LOG_INFO, "replay saved: %s (%zu clip(s) to cut)", job->replay_path, n);
	extract_worker_push(&g.worker, job);
}

/* ------------------------------------------------------------------------ */
/* poller hooks (poller thread)                                             */

static int64_t hook_now(void *ctx)
{
	(void)ctx;
	return (int64_t)os_gettime_ns();
}

static void hook_load_settings(void *ctx, struct poller_settings *ps)
{
	(void)ctx;
	reload_settings();
	EnterCriticalSection(&g.lock);
	snprintf(ps->host, sizeof(ps->host), "%s", g.settings.host);
	ps->port = g.settings.port;
	ps->poll_ms = g.settings.poll_ms;
	ps->clip_unknown_best = g.settings.clip_unknown_best;
	LeaveCriticalSection(&g.lock);
}

static void hook_tick(void *ctx, int64_t now)
{
	struct clip *dropped = NULL;
	size_t num_dropped = 0;
	bool request = false, timed_out = false;
	uint64_t seq = 0;
	size_t pending = 0;
	int64_t latest;

	(void)ctx;
	EnterCriticalSection(&g.lock);
	if (g.save_outstanding) {
		if (now - g.save_queued_ns > SAVE_TIMEOUT_NS) {
			int64_t cut = g.save_t_req_ns ? g.save_t_req_ns : g.save_queued_ns;
			num_dropped = clipq_take_ended_by(&g.queue, cut, &dropped);
			g.save_outstanding = false;
			timed_out = true;
		}
	} else if (clipq_latest_end(&g.queue, &latest)) {
		int64_t margin = (int64_t)(g.settings.save_margin_seconds * (double)NS_PER_SECOND);
		if (now >= latest + margin) {
			g.save_outstanding = true;
			g.save_seq++;
			g.save_queued_ns = now;
			g.save_t_req_ns = 0;
			seq = g.save_seq;
			pending = g.queue.num;
			request = true;
		}
	}
	LeaveCriticalSection(&g.lock);

	if (timed_out) {
		gc_log(GC_LOG_WARNING, "OBS did not report a saved replay within %lld s", SAVE_TIMEOUT_NS / NS_PER_SECOND);
		log_dropped("the replay buffer save did not complete", dropped, num_dropped);
	}
	if (request) {
		gc_log(GC_LOG_DEBUG, "requesting a replay buffer save (%zu clip(s) pending)", pending);
		obs_queue_task(OBS_TASK_UI, ui_save_replay, (void *)(uintptr_t)seq, false);
	}
}

static void hook_run_started(void *ctx, const struct run_info *run)
{
	(void)ctx;
	(void)run;
	obs_queue_task(OBS_TASK_UI, ui_run_started, NULL, false);
}

static void hook_gold(void *ctx, const struct gold_event *ev)
{
	struct clip c;
	bool ok;
	double lead, tail;

	(void)ctx;
	if (!ev->window_known) {
		gc_log(GC_LOG_WARNING, "no clip for split %d \"%s\": its start happened before the plugin saw the run",
		       ev->split_index + 1, ev->split_name);
		return;
	}

	memset(&c, 0, sizeof(c));
	c.run_id = ev->run_id;
	c.split_index = ev->split_index;
	c.game = (char *)ev->game;
	c.category = (char *)ev->category;
	c.split_name = (char *)ev->split_name;
	c.seg_ticks = ev->seg_ticks;

	EnterCriticalSection(&g.lock);
	lead = g.settings.lead_seconds;
	tail = g.settings.tail_seconds;
	clip_window(ev->seg_start_ns, ev->seg_end_ns, lead, tail, &c.start_ns, &c.end_ns);
	ok = clipq_add(&g.queue, &c); /* copies the strings */
	LeaveCriticalSection(&g.lock);

	if (ok)
		gc_log(GC_LOG_INFO, "queued a %.1f s clip for split %d \"%s\"",
		       (double)(c.end_ns - c.start_ns) / (double)NS_PER_SECOND, ev->split_index + 1, ev->split_name);
	else
		gc_log(GC_LOG_ERROR, "out of memory queueing a clip");
}

static void hook_undo(void *ctx, uint64_t run_id, int from_split, int to_split)
{
	size_t n;

	(void)ctx;
	(void)to_split;
	EnterCriticalSection(&g.lock);
	n = clipq_cancel(&g.queue, run_id, from_split);
	LeaveCriticalSection(&g.lock);
	if (n)
		gc_log(GC_LOG_INFO, "cancelled %zu clip(s) of undone splits", n);
}

/* ------------------------------------------------------------------------ */
/* lifetime                                                                 */

static void obs_sink(int level, const char *msg)
{
	blog(level, "[gold-clips] %s", msg);
}

static void start_threads(void)
{
	struct poller_hooks hooks;

	if (g.threads_running)
		return;
	if (!extract_worker_start(&g.worker)) {
		gc_log(GC_LOG_ERROR, "could not start the clip worker thread");
		return;
	}

	memset(&hooks, 0, sizeof(hooks));
	hooks.now_ns = hook_now;
	hooks.load_settings = hook_load_settings;
	hooks.tick = hook_tick;
	hooks.events.run_started = hook_run_started;
	hooks.events.gold = hook_gold;
	hooks.events.undo = hook_undo;
	if (!poller_start(&g.poller, &hooks)) {
		extract_worker_stop(&g.worker, 0);
		return;
	}

	EnterCriticalSection(&g.lock);
	g.threads_running = true;
	LeaveCriticalSection(&g.lock);
}

static void stop_threads(void)
{
	size_t pending;

	EnterCriticalSection(&g.lock);
	g.shutting_down = true;
	if (!g.threads_running) {
		LeaveCriticalSection(&g.lock);
		return;
	}
	g.threads_running = false;
	LeaveCriticalSection(&g.lock);

	/* never join while holding g.lock: both threads take it */
	poller_stop(&g.poller);
	extract_worker_stop(&g.worker, EXIT_GRACE_MS);

	EnterCriticalSection(&g.lock);
	pending = g.queue.num;
	clipq_free(&g.queue);
	g.save_outstanding = false;
	LeaveCriticalSection(&g.lock);
	if (pending)
		gc_log(GC_LOG_WARNING, "%zu gold clip(s) were not saved because OBS is closing", pending);
}

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	(void)data;
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		start_threads();
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		on_replay_saved();
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		EnterCriticalSection(&g.lock);
		g.rb_started_ns = event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED ? (int64_t)os_gettime_ns() : 0;
		LeaveCriticalSection(&g.lock);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		/* The frontend API is torn down right after this event, so
		 * everything that needs it is finished here. */
		stop_threads();
		if (g.callback_registered) {
			obs_frontend_remove_event_callback(on_frontend_event, NULL);
			g.callback_registered = false;
		}
		break;
	default:
		break;
	}
}

static void open_settings(void *data)
{
	struct gc_settings s;
	char *path;
	wchar_t *wpath;

	(void)data;
	read_settings(&s); /* makes sure the file exists */
	path = config_file_path();
	if (!path)
		return;
	wpath = utf8_to_wide(path);
	bfree(path);
	if (!wpath)
		return;
	for (wchar_t *p = wpath; *p; p++)
		if (*p == L'/')
			*p = L'\\';

	if ((INT_PTR)ShellExecuteW(NULL, L"open", wpath, NULL, NULL, SW_SHOWNORMAL) <= 32) {
		/* no program registered for .ini: fall back to Notepad */
		size_t len = wcslen(wpath);
		wchar_t *quoted = malloc((len + 3) * sizeof(wchar_t));
		if (quoted) {
			quoted[0] = L'"';
			memcpy(quoted + 1, wpath, len * sizeof(wchar_t));
			quoted[len + 1] = L'"';
			quoted[len + 2] = L'\0';
			ShellExecuteW(NULL, L"open", L"notepad.exe", quoted, NULL, SW_SHOWNORMAL);
			free(quoted);
		}
	}
	free(wpath);
}

bool obs_module_load(void)
{
	InitializeCriticalSection(&g.lock);
	InitializeCriticalSection(&g.cfg_lock);
	g.locks_ready = true;
	gc_log_set_sink(obs_sink);
	clipq_init(&g.queue);

	gc_log(GC_LOG_INFO, "version %s loaded", PLUGIN_VERSION);
	reload_settings();

	obs_frontend_add_event_callback(on_frontend_event, NULL);
	g.callback_registered = true;
	obs_frontend_add_tools_menu_item("LiveSplit Gold Clips settings", open_settings, NULL);
	return true;
}

void obs_module_unload(void)
{
	if (!g.locks_ready)
		return;
	stop_threads();
	if (g.callback_registered) {
		obs_frontend_remove_event_callback(on_frontend_event, NULL);
		g.callback_registered = false;
	}
	clipq_free(&g.queue);
	gc_log(GC_LOG_INFO, "unloaded");
	gc_log_set_sink(NULL);
	DeleteCriticalSection(&g.cfg_lock);
	DeleteCriticalSection(&g.lock);
	g.locks_ready = false;
}
