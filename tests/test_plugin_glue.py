"""Runs the real plugin (src/plugin.c) on a fake OBS (build/tests/plugin_host.exe)
against a fake LiveSplit server: settings file handling, starting the replay
buffer, the save request / saved event cycle, cutting and deleting, undo
cancelling a queued clip, and a replay buffer that is not enabled.

The plugin's config.ini always points at the fake server's OS-assigned port on
127.0.0.1 before the LiveSplit poller starts.
"""

import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_livesplit import READ_ONLY_COMMANDS, FakeLiveSplit, run_script  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "build", "tests", "plugin_host.exe")
WORK = os.path.join(ROOT, "build", "test-tmp", "glue")

KEYS = ["host", "port", "poll_ms", "lead_seconds", "tail_seconds", "save_margin_seconds", "folder", "ffmpeg",
        "start_replay_buffer", "keep_full_replay", "clip_unknown_best"]

failures = []
checks = 0


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        failures.append(what)
        print("FAIL:", what)


def read_ini(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if "=" in line and not line.startswith("#"):
                key, _, value = line.partition("=")
                values[key] = value
    return values


def write_config(config_dir, port, **overrides):
    os.makedirs(config_dir, exist_ok=True)
    values = {"host": "127.0.0.1", "port": str(port), "poll_ms": "25", "lead_seconds": "0.5",
              "tail_seconds": "0.5", "save_margin_seconds": "0.3", "folder": "Golds", "ffmpeg": "ffmpeg",
              "start_replay_buffer": "true", "keep_full_replay": "false", "clip_unknown_best": "true"}
    values.update(overrides)
    with open(os.path.join(config_dir, "config.ini"), "w", encoding="utf-8", newline="\n") as f:
        f.write("[General]\n")
        for key in KEYS:
            f.write(f"{key}={values[key]}\n")


def run_host(args, timeout):
    proc = subprocess.run([HOST] + args, capture_output=True, timeout=timeout)
    out = proc.stdout.decode("utf-8")
    log = proc.stderr.decode("utf-8", "replace")
    if os.environ.get("GOLD_CLIPS_TEST_VERBOSE"):
        print(out + log)
    summary = {}
    for line in out.splitlines():
        if line.startswith("SUMMARY"):
            for field in line.split("\t")[1:]:
                key, _, value = field.partition("=")
                summary[key] = value
    return proc.returncode, summary, log


def run_with_livesplit(name, fake, script, dirs, seconds, options=()):
    """Starts the host in the background, runs the LiveSplit script once it
    has connected, and returns (exit code, summary, log)."""
    args = [HOST, *dirs, str(seconds), *options]
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        if not fake.connected.wait(5):
            raise RuntimeError(f"{name}: the plugin did not connect")
        time.sleep(0.2)
        run_script(fake, script)
        out, err = proc.communicate(timeout=seconds + 30)
    finally:
        if proc.poll() is None:
            proc.kill()
        fake.close()
    out = out.decode("utf-8")
    log = err.decode("utf-8", "replace")
    if os.environ.get("GOLD_CLIPS_TEST_VERBOSE"):
        print(f"--- {name}\n{out}{log}")
    summary = {}
    for line in out.splitlines():
        if line.startswith("SUMMARY"):
            for field in line.split("\t")[1:]:
                key, _, value = field.partition("=")
                summary[key] = value
    unexpected = [c for c in fake.commands
                  if c not in READ_ONLY_COMMANDS and not (c.startswith("getsplitname ") and c[13:].isdigit())]
    check(not unexpected, f"{name}: unexpected commands {unexpected}")
    return proc.returncode, summary, log


def probe_duration(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                         check=True, capture_output=True, text=True).stdout
    return float(out.strip())


def test_settings_file():
    name = "settings file"
    config_dir = os.path.join(WORK, "settings")
    os.makedirs(config_dir)
    code, summary, log = run_host(["--settings-only", config_dir], 30)
    check(code == 0, f"{name}: exit code {code}\n{log}")
    ini = os.path.join(config_dir, "config.ini")
    check(os.path.exists(ini), f"{name}: config.ini not written")
    if not os.path.exists(ini):
        return
    text = open(ini, encoding="utf-8").read()
    values = read_ini(ini)
    check(list(values) == KEYS, f"{name}: keys {list(values)}")
    check(values.get("port") == "16834" and values.get("folder") == "Golds" and values.get("poll_ms") == "25",
          f"{name}: defaults {values}")
    check(text.count("\n# ") >= 8 and text.startswith("# LiveSplit Gold Clips settings"),
          f"{name}: comments missing:\n{text}")
    check("wrote default settings to" in log, f"{name}: log\n{log}")
    check("settings: LiveSplit 127.0.0.1:16834, poll 25 ms, lead 2.0 s, tail 2.0 s" in log, f"{name}: log\n{log}")
    check(summary.get("tools_item") == "LiveSplit Gold Clips settings", f"{name}: tools menu {summary}")
    check(summary.get("callback_removed") == "1", f"{name}: event callback not removed {summary}")

    # a file missing a key gets it added; odd values fall back to defaults
    with open(ini, "w", encoding="utf-8", newline="\n") as f:
        f.write("[General]\nport=70000\npoll_ms=1\nlead_seconds=abc\nfolder=\nffmpeg=  C:/ff/ffmpeg.exe  \n")
    code, summary, log = run_host(["--settings-only", config_dir], 30)
    values = read_ini(ini)
    check(set(values) == set(KEYS), f"{name}: keys after adding the missing ones {list(values)}")
    check(values.get("tail_seconds") == "2.0", f"{name}: added default {values}")
    check(summary.get("config_saves") == "1", f"{name}: summary {summary}")
    check('LiveSplit 127.0.0.1:16834, poll 5 ms, lead 0.0 s' in log and 'folder "", ffmpeg "C:/ff/ffmpeg.exe"' in log,
          f"{name}: sanitised values\n{log}")


def sample_video(path):
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
        "-t", "20", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
        "-g", "60", "-keyint_min", "60", "-sc_threshold", "0", "-c:a", "aac", path,
    ], check=True)


def test_save_cycle(sample):
    name = "save cycle"
    config_dir = os.path.join(WORK, "cycle config")
    rec = os.path.join(WORK, "cycle rec")
    os.makedirs(rec)
    fake = FakeLiveSplit("Glue Game", "Any%", ["One", "Two", "Three"], [5.0, 5.0, 5.0])
    write_config(config_dir, fake.port)
    script = [
        (0.3, "start"),
        (0.8, "split", 4.0),    # gold: saved about 0.8 s later
        (1.3, "split", 10.0),   # 6 > 5
        (2.0, "split", 14.0),   # gold on the final split ...
        (2.4, "undo"),          # ... undone before its save is due
        (2.6, "split", 14.5),   # gold again
        (3.0, "reset"),
    ]
    code, summary, log = run_with_livesplit(name, fake, script, [config_dir, rec, sample], 6)
    check(code == 0, f"{name}: exit code {code}\n{log}")
    check(summary.get("rb_starts") == "1", f"{name}: replay buffer starts {summary}")
    check(summary.get("saves") == "2" and summary.get("save_requests") == "2", f"{name}: saves {summary}")
    check(summary.get("callback_removed") == "1", f"{name}: callback not removed at exit {summary}")

    clips = os.path.join(rec, "Golds", "Glue Game", "Any%")
    got = sorted(os.listdir(clips)) if os.path.isdir(clips) else []
    check(got == ["One 4.00s.mkv", "Three 4.50s.mkv"], f"{name}: clips {got}\n{log}")
    # The plugin started the replay buffer at the run start, so the first
    # clip's lead (0.5 s before the start) is not in the buffer: it keeps its
    # end and loses the lead (1.0 s instead of 1.5 s).
    for clip, window in (("One 4.00s.mkv", 1.0), ("Three 4.50s.mkv", 2.3)):
        path = os.path.join(clips, clip)
        if os.path.exists(path):
            d = probe_duration(path)
            check(window - 0.2 <= d <= window + 2.2, f"{name}: {clip} lasts {d:.2f} s, window {window} s")
    leftovers = [f for f in os.listdir(rec) if f != "Golds"]
    check(not leftovers, f"{name}: full replays not deleted: {leftovers}")
    check("starting the replay buffer (60 s)" in log, f"{name}: log\n{log}")
    check('so the clip for split 1 "One" starts late' in log, f"{name}: log\n{log}")
    check(log.count("starts late: the saved replay only reaches back") == 1, f"{name}: log\n{log}")
    check(log.count("saving the replay buffer for 1 clip(s)") == 2, f"{name}: log\n{log}")
    check("cancelled 1 clip(s) of undone splits" in log, f"{name}: log\n{log}")
    check(log.count("clip written:") == 2 and log.count("deleted the full replay") == 2, f"{name}: log\n{log}")
    check("[gold-clips]" in log and "error:" not in log, f"{name}: errors in log\n{log}")


def test_replay_buffer_disabled(sample):
    name = "replay buffer disabled"
    config_dir = os.path.join(WORK, "disabled config")
    rec = os.path.join(WORK, "disabled rec")
    os.makedirs(rec)
    fake = FakeLiveSplit("Glue Game", "Any%", ["One", "Two"], [5.0, 5.0])
    write_config(config_dir, fake.port)
    script = [(0.3, "start"), (0.8, "split", 4.0), (1.0, "reset")]
    code, summary, log = run_with_livesplit(name, fake, script, [config_dir, rec, sample], 3, ["--rb-disabled"])
    check(code == 0, f"{name}: exit code {code}")
    check(summary.get("saves") == "0" and summary.get("save_requests") == "0", f"{name}: {summary}")
    check("the replay buffer is not enabled" in log, f"{name}: log\n{log}")
    check("the replay buffer is not active, so 1 clip(s) cannot be saved" in log, f"{name}: log\n{log}")
    check('dropped the clip for split 1 "One"' in log, f"{name}: log\n{log}")
    check(os.listdir(rec) == [], f"{name}: files written {os.listdir(rec)}")


def test_short_buffer_warning(sample):
    name = "short buffer (advanced output mode)"
    config_dir = os.path.join(WORK, "short config")
    rec = os.path.join(WORK, "short rec")
    os.makedirs(rec)
    fake = FakeLiveSplit("Glue Game", "Any%", ["One", "Two"], [5.0, 5.0])
    write_config(config_dir, fake.port, lead_seconds="2.0", tail_seconds="0.2", save_margin_seconds="0.1")
    script = [(0.3, "start"), (0.8, "split", 4.0), (1.5, "reset")]
    code, summary, log = run_with_livesplit(
        name, fake, script, [config_dir, rec, sample], 3, ["--advanced", "--rb-seconds", "2", "--rb-active"])
    check(code == 0, f"{name}: exit code {code}")
    check(summary.get("rb_starts") == "0", f"{name}: the active buffer was started again {summary}")
    check("replay buffer is running (2 s)" in log, f"{name}: AdvOut RecRBTime not read\n{log}")
    check("but the buffer holds 2 s; it will start late" in log, f"{name}: log\n{log}")
    check("starts late: the saved replay only reaches back 2.0 s" in log, f"{name}: log\n{log}")
    clip = os.path.join(rec, "Golds", "Glue Game", "Any%", "One 4.00s.mkv")
    check(os.path.exists(clip), f"{name}: clip missing\n{log}")


def main():
    if not os.path.exists(HOST):
        print(f"missing {HOST}; run ./build.sh first")
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    sample = os.path.join(WORK, "sample.mkv")
    sample_video(sample)

    test_settings_file()
    test_save_cycle(sample)
    test_replay_buffer_disabled(sample)
    test_short_buffer_warning(sample)
    print(f"plugin glue tests: 4 cases, {checks} checks, {len(failures)} failed")
    if not failures:
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
