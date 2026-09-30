#!/usr/bin/env bash
# Builds build/livesplit-gold-clips.dll and the test programs (Git Bash on Windows).
#
#   TOOLCHAIN  folder with clang, llvm-readobj and dlltool (x86_64-w64-windows-gnu)
#   OBS_BIN    OBS's bin/64bit folder; import libraries are generated from its DLLs
set -euo pipefail
cd "$(dirname "$0")"

TOOLCHAIN="${TOOLCHAIN:-/c/ghcup/ghc/9.4.7/mingw/bin}"
OBS_BIN="${OBS_BIN:-/c/Program Files/obs-studio/bin/64bit}"
export PATH="$TOOLCHAIN:$PATH"

if [ ! -f deps/obs-studio/libobs/obs-module.h ]; then
	echo "OBS headers missing: run tools/fetch-obs-headers.sh first" >&2
	exit 1
fi

CFLAGS=(-std=c11 -O2 -Wall -Wextra -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -Isrc)
OBS_CFLAGS=(-Iinclude -Ideps/obs-studio/libobs -Ideps/obs-studio/frontend/api)
LDFLAGS=(-static-libgcc -s)
SYSLIBS=(-lws2_32 -lshell32)

CORE=(
	src/gclog.c
	src/lstime.c
	src/tracker.c
	src/clips.c
	src/strbuf.c
	src/winutil.c
	src/procrun.c
	src/extract.c
	src/lsclient.c
	src/poller.c
)

mkdir -p build/lib build/tests

# Import library for one of OBS's DLLs, from its export table (C names only).
implib() {
	local dll="$1" name="$2"
	local def="build/lib/$name.def" lib="build/lib/lib$name.a"
	if [ -f "$lib" ] && [ "$lib" -nt "$OBS_BIN/$dll" ] && [ "$lib" -nt build.sh ]; then
		return
	fi
	echo "generating $lib from $dll"
	{
		echo "LIBRARY $dll"
		echo "EXPORTS"
		llvm-readobj --coff-exports "$OBS_BIN/$dll" | sed -n 's/^ *Name: \([A-Za-z_][A-Za-z0-9_]*\)$/\1/p'
	} >"$def"
	dlltool -d "$def" -l "$lib" -D "$dll" -m i386:x86-64
}

implib obs.dll obs
implib obs-frontend-api.dll obs-frontend-api

echo "building build/livesplit-gold-clips.dll"
clang "${CFLAGS[@]}" "${OBS_CFLAGS[@]}" -shared -o build/livesplit-gold-clips.dll \
	src/plugin.c "${CORE[@]}" src/plugin.def -Lbuild/lib -lobs -lobs-frontend-api "${SYSLIBS[@]}" "${LDFLAGS[@]}"

echo "building test programs"
clang "${CFLAGS[@]}" -o build/tests/unit_tests.exe tests/unit_tests.c "${CORE[@]}" "${SYSLIBS[@]}" "${LDFLAGS[@]}"
clang "${CFLAGS[@]}" -o build/tests/ls_harness.exe tests/ls_harness.c "${CORE[@]}" "${SYSLIBS[@]}" "${LDFLAGS[@]}"
clang "${CFLAGS[@]}" -o build/tests/extract_tool.exe tests/extract_tool.c "${CORE[@]}" "${SYSLIBS[@]}" "${LDFLAGS[@]}"
# the real plugin.c on a fake OBS (tests/fake_obs.c)
clang "${CFLAGS[@]}" "${OBS_CFLAGS[@]}" -Itests -o build/tests/plugin_host.exe tests/plugin_host.c tests/fake_obs.c \
	src/plugin.c "${CORE[@]}" "${SYSLIBS[@]}" "${LDFLAGS[@]}"

echo "build OK"
