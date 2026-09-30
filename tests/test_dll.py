"""Checks the plugin DLL's exports and imports: exactly the OBS module entry
points are exported, and it only needs OBS's own DLLs, system DLLs and the
Universal CRT (no libgcc, libwinpthread or libc++ from the toolchain).

Needs llvm-readobj on PATH (tests/run.sh puts the toolchain there).
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(ROOT, "build", "livesplit-gold-clips.dll")

EXPORTS = {
    "obs_module_load",
    "obs_module_unload",
    "obs_module_set_pointer",
    "obs_current_module",
    "obs_module_ver",
    "obs_module_name",
    "obs_module_description",
}
ALLOWED_IMPORTS = {"obs.dll", "obs-frontend-api.dll", "kernel32.dll", "ws2_32.dll", "shell32.dll"}


def readobj(flag):
    return subprocess.run(["llvm-readobj", flag, DLL], check=True, capture_output=True, text=True).stdout


def main():
    failures = []
    exports = {n for n in re.findall(r"^\s*Name: (\S+)$", readobj("--coff-exports"), re.M)}
    imports = re.findall(r"^\s*Name: (\S+\.dll)$", readobj("--coff-imports"), re.M | re.I)

    if exports != EXPORTS:
        failures.append(f"exports {sorted(exports)}, expected {sorted(EXPORTS)}")
    bad = [d for d in imports if d.lower() not in ALLOWED_IMPORTS and not d.lower().startswith("api-ms-win-crt-")]
    if bad:
        failures.append(f"unexpected imports: {bad}")
    for dll in ("obs.dll", "obs-frontend-api.dll"):
        if dll not in [d.lower() for d in imports]:
            failures.append(f"{dll} is not imported")

    print(f"plugin DLL: {os.path.getsize(DLL)} bytes, exports {len(exports)}, imports {', '.join(imports)}")
    for f in failures:
        print("FAIL:", f)
    print(f"DLL checks: 3 checks, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
