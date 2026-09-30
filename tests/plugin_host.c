/*
 * Runs the real plugin (src/plugin.c) against fake_obs.c: loads the module,
 * sends FINISHED_LOADING, runs the fake UI thread for a while, then exits
 * the way OBS does (EXIT event, module unload) and prints a summary line.
 *
 *   plugin_host --settings-only <config_dir>
 *   plugin_host <config_dir> <rec_dir> <sample_video> <seconds>
 *               [--rb-seconds N] [--advanced] [--rb-disabled] [--rb-active]
 *
 * The plugin reads <config_dir>/config.ini. Unless --settings-only is given
 * (the LiveSplit poller never starts then), the file must already point the
 * plugin at a test server: a port other than LiveSplit's 16834 is required.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_obs.h"
#include "winutil.h"

static bool config_points_at_test_server(const char *config_dir)
{
	char path[1200], line[512];
	bool ok = false;
	FILE *f;

	snprintf(path, sizeof(path), "%s/config.ini", config_dir);
	f = fopen(path, "rb");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "port=", 5) == 0)
			ok = atoi(line + 5) > 0 && atoi(line + 5) != 16834;
	}
	fclose(f);
	return ok;
}

static void summary(void)
{
	printf("SUMMARY\trb_starts=%d\tsave_requests=%d\tsaves=%d\tcallback_removed=%d\tconfig_saves=%d\ttools_item=%s\n",
	       fake_obs.rb_starts, fake_obs.save_requests, fake_obs.saves, fake_obs.callback_removed,
	       fake_obs.config_saves, fake_obs.tools_item);
}

int main(int argc, char **argv)
{
	ULONGLONG deadline;

	if (argc == 3 && strcmp(argv[1], "--settings-only") == 0) {
		snprintf(fake_obs.config_dir, sizeof(fake_obs.config_dir), "%s", argv[2]);
		strcpy(fake_obs.rb_seconds_text, "20");
		fake_obs_init();
		if (!obs_module_load())
			return 1;
		obs_module_unload();
		summary();
		return 0;
	}
	if (argc < 5) {
		fprintf(stderr, "usage: see plugin_host.c\n");
		return 2;
	}

	snprintf(fake_obs.config_dir, sizeof(fake_obs.config_dir), "%s", argv[1]);
	snprintf(fake_obs.rec_dir, sizeof(fake_obs.rec_dir), "%s", argv[2]);
	snprintf(fake_obs.sample_video, sizeof(fake_obs.sample_video), "%s", argv[3]);
	strcpy(fake_obs.rb_seconds_text, "60");
	fake_obs.rb_enabled = true;
	fake_obs.rb_can_start = true;
	for (int i = 5; i < argc; i++) {
		if (strcmp(argv[i], "--rb-seconds") == 0 && i + 1 < argc)
			snprintf(fake_obs.rb_seconds_text, sizeof(fake_obs.rb_seconds_text), "%s", argv[++i]);
		else if (strcmp(argv[i], "--advanced") == 0)
			fake_obs.advanced_output = true;
		else if (strcmp(argv[i], "--rb-disabled") == 0)
			fake_obs.rb_enabled = false;
		else if (strcmp(argv[i], "--rb-active") == 0)
			fake_obs.rb_active = true;
	}
	if (!config_points_at_test_server(fake_obs.config_dir)) {
		fprintf(stderr, "refusing to run: %s/config.ini must set a test port (not 16834)\n", fake_obs.config_dir);
		return 2;
	}

	fake_obs_init();
	if (!obs_module_load())
		return 1;
	fake_obs_event(OBS_FRONTEND_EVENT_FINISHED_LOADING);

	deadline = GetTickCount64() + (ULONGLONG)(atof(argv[4]) * 1000);
	while (GetTickCount64() < deadline) {
		fake_obs_run_ui();
		Sleep(5);
	}

	fake_obs_event(OBS_FRONTEND_EVENT_EXIT);
	obs_module_unload();
	summary();
	return 0;
}
