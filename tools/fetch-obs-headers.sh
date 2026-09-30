#!/usr/bin/env bash
# Fetches the OBS 32.0.4 headers the plugin is built against into deps/obs-studio
# (a sparse clone with only libobs and the frontend API).
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -d deps/obs-studio ]; then
	echo "deps/obs-studio already exists"
	exit 0
fi
git clone --depth 1 --filter=blob:none --sparse --branch 32.0.4 https://github.com/obsproject/obs-studio.git deps/obs-studio
git -C deps/obs-studio sparse-checkout set libobs frontend/api
