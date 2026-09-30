/*
 * Stand-in for the obsconfig.h OBS generates at configure time. obs-config.h
 * includes it; a plugin built outside the OBS tree only needs the release
 * build values, and the paths are never used by plugin code.
 */
#pragma once

#define OBS_DATA_PATH "../../data"
#define OBS_PLUGIN_PATH "../../obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"
#define OBS_INSTALL_PREFIX ""

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
