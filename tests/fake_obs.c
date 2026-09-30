/*
 * Just enough of libobs and the OBS frontend API to run src/plugin.c outside
 * OBS (see plugin_host.c). The prototypes come from the real OBS headers, so
 * the plugin is compiled against the same declarations as for OBS.
 *
 * The "UI thread" is the host's main thread: obs_queue_task(OBS_TASK_UI)
 * queues work for fake_obs_run_ui(), which also plays the replay buffer:
 * a save request is answered a little later by copying a sample video into
 * the recording folder and firing OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_obs.h"
#include "winutil.h"

struct fake_obs_state fake_obs;

static CRITICAL_SECTION task_lock;
static struct task {
	obs_task_t fn;
	void *param;
} tasks[256];
static size_t num_tasks;

static obs_frontend_event_cb event_cb;
static void *event_data;
static char *last_replay;
static int64_t save_due_ns;
static bool started_event_pending;
static int output_dummy;

/* ---- libobs --------------------------------------------------------- */

void bfree(void *ptr)
{
	free(ptr);
}

void blogva(int log_level, const char *format, va_list args)
{
	const char *tag = log_level <= LOG_ERROR ? "error" : log_level <= LOG_WARNING ? "warning"
						 : log_level <= LOG_INFO ? "info" : "debug";
	char buf[4096];
	vsnprintf(buf, sizeof(buf), format, args);
	fprintf(stderr, "%s: %s\n", tag, buf);
	fflush(stderr);
}

void blog(int log_level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	blogva(log_level, format, args);
	va_end(args);
}

uint64_t os_gettime_ns(void)
{
	return (uint64_t)qpc_now_ns();
}

FILE *os_fopen(const char *path, const char *mode)
{
	wchar_t *wpath = utf8_to_wide(path), *wmode = utf8_to_wide(mode);
	FILE *f = wpath && wmode ? _wfopen(wpath, wmode) : NULL;
	free(wpath);
	free(wmode);
	return f;
}

int os_mkdirs(const char *path)
{
	return fs_mkdirs(path) ? MKDIR_SUCCESS : MKDIR_ERROR;
}

char *obs_module_get_config_path(obs_module_t *module, const char *file)
{
	size_t len = strlen(fake_obs.config_dir) + strlen(file) + 2;
	char *path = malloc(len);
	(void)module;
	snprintf(path, len, "%s/%s", fake_obs.config_dir, file);
	return path;
}

void obs_output_release(obs_output_t *output)
{
	(void)output;
}

void obs_queue_task(enum obs_task_type type, obs_task_t task, void *param, bool wait)
{
	(void)wait;
	if (type != OBS_TASK_UI) {
		fprintf(stderr, "fake_obs: unexpected task type %d\n", (int)type);
		abort();
	}
	EnterCriticalSection(&task_lock);
	if (num_tasks < sizeof(tasks) / sizeof(tasks[0])) {
		tasks[num_tasks].fn = task;
		tasks[num_tasks].param = param;
		num_tasks++;
	}
	LeaveCriticalSection(&task_lock);
}

/* ---- a small ini store with libobs' parsing rules ------------------- */

struct config_item {
	char *section;
	char *name;
	char *value;
};

struct config_data {
	char *file;
	struct config_item items[128];
	size_t num;
};

static struct config_item *find_item(config_t *cfg, const char *section, const char *name)
{
	for (size_t i = 0; i < cfg->num; i++)
		if (strcmp(cfg->items[i].section, section) == 0 && strcmp(cfg->items[i].name, name) == 0)
			return &cfg->items[i];
	return NULL;
}

void config_set_string(config_t *cfg, const char *section, const char *name, const char *value)
{
	struct config_item *item = find_item(cfg, section, name);
	if (item) {
		free(item->value);
		item->value = _strdup(value);
		return;
	}
	if (cfg->num < sizeof(cfg->items) / sizeof(cfg->items[0])) {
		cfg->items[cfg->num].section = _strdup(section);
		cfg->items[cfg->num].name = _strdup(name);
		cfg->items[cfg->num].value = _strdup(value);
		cfg->num++;
	}
}

int config_open(config_t **config, const char *file, enum config_open_type open_type)
{
	FILE *f = os_fopen(file, "rb");
	char line[1024], section[256] = "";

	if (!f && open_type == CONFIG_OPEN_ALWAYS)
		f = os_fopen(file, "w+");
	if (!f)
		return CONFIG_FILENOTFOUND;
	*config = calloc(1, sizeof(config_t));
	(*config)->file = _strdup(file);
	fake_obs.config_opens++;

	while (fgets(line, sizeof(line), f)) {
		char *eq, *p = line;
		line[strcspn(line, "\r\n")] = '\0';
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p || *p == '#')
			continue;
		if (*p == '[') {
			char *end = strchr(p, ']');
			if (end) {
				*end = '\0';
				snprintf(section, sizeof(section), "%s", p + 1);
			}
			continue;
		}
		eq = strchr(p, '=');
		if (!eq || !section[0])
			continue;
		*eq = '\0';
		config_set_string(*config, section, p, eq + 1); /* like libobs: no trimming */
	}
	fclose(f);
	return CONFIG_SUCCESS;
}

int config_save_safe(config_t *cfg, const char *temp_ext, const char *backup_ext)
{
	FILE *f = os_fopen(cfg->file, "wb");
	(void)temp_ext;
	(void)backup_ext;
	if (!f)
		return CONFIG_ERROR;
	for (size_t i = 0; i < cfg->num; i++) {
		bool first = true;
		for (size_t j = 0; j < i; j++)
			if (strcmp(cfg->items[j].section, cfg->items[i].section) == 0)
				first = false;
		if (!first)
			continue;
		fprintf(f, "[%s]\n", cfg->items[i].section);
		for (size_t j = i; j < cfg->num; j++)
			if (strcmp(cfg->items[j].section, cfg->items[i].section) == 0)
				fprintf(f, "%s=%s\n", cfg->items[j].name, cfg->items[j].value);
	}
	fclose(f);
	fake_obs.config_saves++;
	return CONFIG_SUCCESS;
}

void config_close(config_t *cfg)
{
	if (!cfg)
		return;
	for (size_t i = 0; i < cfg->num; i++) {
		free(cfg->items[i].section);
		free(cfg->items[i].name);
		free(cfg->items[i].value);
	}
	free(cfg->file);
	free(cfg);
}

bool config_has_user_value(config_t *cfg, const char *section, const char *name)
{
	return find_item(cfg, section, name) != NULL;
}

const char *config_get_string(config_t *cfg, const char *section, const char *name)
{
	struct config_item *item = find_item(cfg, section, name);
	return item ? item->value : NULL;
}

int64_t config_get_int(config_t *cfg, const char *section, const char *name)
{
	const char *v = config_get_string(cfg, section, name);
	return v ? strtoll(v, NULL, 10) : 0;
}

double config_get_double(config_t *cfg, const char *section, const char *name)
{
	const char *v = config_get_string(cfg, section, name);
	return v ? strtod(v, NULL) : 0.0;
}

bool config_get_bool(config_t *cfg, const char *section, const char *name)
{
	const char *v = config_get_string(cfg, section, name);
	return v && (_stricmp(v, "true") == 0 || strtoull(v, NULL, 10) != 0);
}

/* ---- frontend -------------------------------------------------------- */

static config_t profile;

void obs_frontend_add_event_callback(obs_frontend_event_cb callback, void *private_data)
{
	event_cb = callback;
	event_data = private_data;
}

void obs_frontend_remove_event_callback(obs_frontend_event_cb callback, void *private_data)
{
	if (event_cb == callback && event_data == private_data) {
		event_cb = NULL;
		fake_obs.callback_removed++;
	}
}

void obs_frontend_add_tools_menu_item(const char *name, obs_frontend_cb callback, void *private_data)
{
	(void)callback;
	(void)private_data;
	snprintf(fake_obs.tools_item, sizeof(fake_obs.tools_item), "%s", name);
}

config_t *obs_frontend_get_profile_config(void)
{
	return &profile;
}

obs_output_t *obs_frontend_get_replay_buffer_output(void)
{
	return fake_obs.rb_enabled ? (obs_output_t *)&output_dummy : NULL;
}

bool obs_frontend_replay_buffer_active(void)
{
	return fake_obs.rb_active;
}

void obs_frontend_replay_buffer_start(void)
{
	fake_obs.rb_starts++;
	if (fake_obs.rb_enabled && fake_obs.rb_can_start && !fake_obs.rb_active) {
		fake_obs.rb_active = true;
		started_event_pending = true; /* OBS reports it once the output runs */
	}
}

void obs_frontend_replay_buffer_save(void)
{
	if (!fake_obs.rb_active)
		return;
	fake_obs.save_requests++;
	save_due_ns = qpc_now_ns() + 200 * 1000000LL; /* muxing takes a moment */
}

char *obs_frontend_get_last_replay(void)
{
	return _strdup(last_replay ? last_replay : "");
}

/* ---- host side ------------------------------------------------------- */

void fake_obs_init(void)
{
	InitializeCriticalSection(&task_lock);
	config_set_string(&profile, "Output", "Mode", fake_obs.advanced_output ? "Advanced" : "Simple");
	config_set_string(&profile, fake_obs.advanced_output ? "AdvOut" : "SimpleOutput", "RecRBTime",
			  fake_obs.rb_seconds_text);
}

void fake_obs_event(enum obs_frontend_event event)
{
	if (event_cb)
		event_cb(event, event_data);
}

void fake_obs_run_ui(void)
{
	for (;;) {
		struct task t;
		EnterCriticalSection(&task_lock);
		if (!num_tasks) {
			LeaveCriticalSection(&task_lock);
			break;
		}
		t = tasks[0];
		memmove(tasks, tasks + 1, (num_tasks - 1) * sizeof(tasks[0]));
		num_tasks--;
		LeaveCriticalSection(&task_lock);
		t.fn(t.param);
	}

	if (started_event_pending) {
		started_event_pending = false;
		fake_obs_event(OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED);
	}

	if (save_due_ns && qpc_now_ns() >= save_due_ns) {
		char path[1024];
		wchar_t *src, *dst;

		save_due_ns = 0;
		if (fake_obs.fail_saves)
			return; /* OBS never reports the save */
		snprintf(path, sizeof(path), "%s/Replay %d.mkv", fake_obs.rec_dir, ++fake_obs.saves);
		src = utf8_to_wide(fake_obs.sample_video);
		dst = utf8_to_wide(path);
		if (!src || !dst || !CopyFileW(src, dst, FALSE))
			fprintf(stderr, "fake_obs: cannot copy the sample replay\n");
		free(src);
		free(dst);
		free(last_replay);
		last_replay = _strdup(path);
		fake_obs_event(OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED);
	}
}
