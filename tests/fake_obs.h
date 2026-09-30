/* Controls for the fake OBS in fake_obs.c. */
#pragma once

#include <obs-frontend-api.h>

#include <stdbool.h>

struct fake_obs_state {
	char config_dir[1024];    /* obs_module_config_path() root */
	char rec_dir[1024];       /* where "saved replays" go */
	char sample_video[1024];  /* copied as each saved replay */
	char rb_seconds_text[16]; /* profile RecRBTime */
	bool advanced_output;     /* profile Output/Mode */
	bool rb_enabled;          /* replay buffer enabled in the profile */
	bool rb_can_start;        /* obs_frontend_replay_buffer_start() works */
	bool rb_active;
	bool fail_saves; /* never report a saved replay */

	int rb_starts;
	int save_requests;
	int saves;
	int config_opens;
	int config_saves;
	int callback_removed;
	char tools_item[256];
};

extern struct fake_obs_state fake_obs;

void fake_obs_init(void);
void fake_obs_event(enum obs_frontend_event event);
/* Runs queued UI tasks and finishes a pending replay save. Main thread only. */
void fake_obs_run_ui(void);
