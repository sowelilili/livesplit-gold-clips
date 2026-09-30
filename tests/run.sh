#!/usr/bin/env bash
# Builds everything and runs every test. Needs ffmpeg and ffprobe on PATH.
# The LiveSplit tests only talk to fake servers on 127.0.0.1 ports the OS
# assigns; nothing connects to a real LiveSplit or touches OBS.
set -euo pipefail
cd "$(dirname "$0")/.."

TOOLCHAIN="${TOOLCHAIN:-/c/ghcup/ghc/9.4.7/mingw/bin}"
PYTHON="${PYTHON:-python}"
export PATH="$TOOLCHAIN:$PATH"

./build.sh

echo
echo "== unit tests"
build/tests/unit_tests.exe

echo
echo "== plugin DLL exports and imports"
"$PYTHON" tests/test_dll.py

echo
echo "== LiveSplit client against a fake LiveSplit server"
"$PYTHON" tests/test_livesplit.py

echo
echo "== clip extraction with ffmpeg"
"$PYTHON" tests/test_extract.py

echo
echo "== the plugin on a fake OBS"
"$PYTHON" tests/test_plugin_glue.py

echo
echo "all tests passed"
