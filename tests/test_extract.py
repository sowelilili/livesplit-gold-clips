"""Runs the plugin's real extraction code (build/tests/extract_tool.exe) on a
generated sample video and checks the clips with ffprobe: file names and
folders, durations (the requested length plus at most one keyframe interval,
since cuts start on the keyframe at or before the requested start), streams,
unique-name suffixes, and whether the full replay is deleted or kept.
"""

import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "build", "tests", "extract_tool.exe")
WORK = os.path.join(ROOT, "build", "test-tmp", "extract")
KEYFRAME_INTERVAL = 2.0
SAMPLE_SECONDS = 30

failures = []
checks = 0


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        failures.append(what)
        print("FAIL:", what)


def make_sample(path):
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
        "-t", str(SAMPLE_SECONDS),
        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
        "-g", "60", "-keyint_min", "60", "-sc_threshold", "0",  # a keyframe every 2 s
        "-c:a", "aac",
        path,
    ], check=True)


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration:stream=codec_type",
                          "-of", "json", path], check=True, capture_output=True).stdout
    info = json.loads(out)
    return float(info["format"]["duration"]), sorted(s["codec_type"] for s in info["streams"])


def run_tool(replay, folder, keep, buffer_seconds, clips, ffmpeg="ffmpeg"):
    args = [TOOL, replay, folder, "1" if keep else "0", ffmpeg, str(buffer_seconds)]
    for clip in clips:
        args += [str(v) for v in clip]
    proc = subprocess.run(args, capture_output=True)
    log = proc.stderr.decode("utf-8", "replace")
    if os.environ.get("GOLD_CLIPS_TEST_VERBOSE"):
        print(log)
    return proc.returncode, log


def fresh_replay(sample, folder, name="Replay 2026-09-30 18-00-00.mkv"):
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, name)
    shutil.copyfile(sample, path)
    return path


def check_clip(label, path, length):
    check(os.path.isfile(path), f"{label}: missing {path}")
    if not os.path.isfile(path):
        return
    duration, streams = probe(path)
    check(length - 0.15 <= duration <= length + KEYFRAME_INTERVAL + 0.15,
          f"{label}: duration {duration:.3f} s, expected {length} s to {length + KEYFRAME_INTERVAL} s")
    check(streams == ["audio", "video"], f"{label}: streams {streams}")


def test_basic(sample):
    label = "basic"
    rec = os.path.join(WORK, "rec one")
    replay = fresh_replay(sample, rec)
    game, category, split = "Ratchet & Clank: Going Commando", "Any% <NG+>", "Gr\u00f6\u00dfe? Boss"
    code, log = run_tool(replay, "Golds", False, 0, [
        (game, category, split, 83.45, 12.3, 5.0),
        (game, category, split, 83.45, 20.0, 4.0),  # same name: gets " (2)"
        ("CON", "cat.", "nul", 3.004, 6.0, 3.0),
    ])
    check(code == 0, f"{label}: exit code {code}\n{log}")
    folder = os.path.join(rec, "Golds", "Ratchet & Clank_ Going Commando", "Any% _NG+_")
    check_clip(label, os.path.join(folder, "Gr\u00f6\u00dfe_ Boss 1m23.45s.mkv"), 5.0)
    check_clip(label, os.path.join(folder, "Gr\u00f6\u00dfe_ Boss 1m23.45s (2).mkv"), 4.0)
    check_clip(label, os.path.join(rec, "Golds", "_CON", "cat", "_nul 3.00s.mkv"), 3.0)
    check(sorted(os.listdir(folder)) == ["Gr\u00f6\u00dfe_ Boss 1m23.45s (2).mkv", "Gr\u00f6\u00dfe_ Boss 1m23.45s.mkv"],
          f"{label}: files {os.listdir(folder)}")
    check(not os.path.exists(replay), f"{label}: the full replay was not deleted")
    check(log.count("clip written:") == 3, f"{label}: log\n{log}")
    check("deleted the full replay" in log, f"{label}: log\n{log}")

    # a second save with the same split name continues the numbering
    replay = fresh_replay(sample, rec)
    code, log = run_tool(replay, "Golds", False, 0, [(game, category, split, 83.45, 8.0, 2.5)])
    check(code == 0, f"{label}: second run exit code {code}\n{log}")
    check_clip(label, os.path.join(folder, "Gr\u00f6\u00dfe_ Boss 1m23.45s (3).mkv"), 2.5)


def test_keep_and_buffer_clamp(sample):
    label = "keep + buffer clamp"
    rec = os.path.join(WORK, "rec two")
    replay = fresh_replay(sample, rec)
    # window 15 s before the end, 8 s long, but the buffer holds 10 s: the
    # clip starts at the buffer start and keeps its end, so it is 3 s long
    code, log = run_tool(replay, "Clips/Sub", True, 10, [("Game", "Cat", "Long", 754.321, 15.0, 8.0)])
    check(code == 0, f"{label}: exit code {code}\n{log}")
    check_clip(label, os.path.join(rec, "Clips", "Sub", "Game", "Cat", "Long 12m34.32s.mkv"), 3.0)
    check(os.path.exists(replay), f"{label}: keep_full_replay did not keep the replay")
    check("starts late" in log, f"{label}: no late-start warning\n{log}")


def test_failures(sample):
    label = "missing ffmpeg"
    rec = os.path.join(WORK, "rec three")
    replay = fresh_replay(sample, rec)
    code, log = run_tool(replay, "Golds", False, 0, [("Game", "Cat", "A", 1.0, 5.0, 2.0)],
                         ffmpeg="C:/no/such/dir/ffmpeg.exe")
    check(code == 1, f"{label}: exit code {code}")
    check("could not start ffmpeg" in log, f"{label}: log\n{log}")
    check(os.path.exists(replay), f"{label}: the replay must be kept when a clip fails")

    label = "broken replay"
    broken = os.path.join(rec, "broken.mkv")
    with open(broken, "w") as f:
        f.write("not a video\n")
    code, log = run_tool(broken, "Golds", False, 0, [("Game", "Cat", "B", 1.0, 5.0, 2.0)])
    check(code == 1, f"{label}: exit code {code}")
    check("ffmpeg failed (exit code" in log and "ffmpeg: " in log, f"{label}: ffmpeg's error not logged\n{log}")
    check(os.path.exists(broken), f"{label}: the replay must be kept when a clip fails")
    check(not os.path.exists(os.path.join(rec, "Golds", "Game", "Cat", "B 1.00s.mkv")),
          f"{label}: partial output left behind")

    label = "one good, one too old"
    code, log = run_tool(replay, "Golds", False, 5, [
        ("Game", "Cat", "Good", 2.0, 4.0, 3.0),
        ("Game", "Cat", "Old", 2.0, 20.0, 3.0),  # ends 17 s before the save; buffer holds 5 s
    ])
    check(code == 1, f"{label}: exit code {code}")
    check_clip(label, os.path.join(rec, "Golds", "Game", "Cat", "Good 2.00s.mkv"), 3.0)
    check("older than" in log, f"{label}: log\n{log}")
    check(os.path.exists(replay), f"{label}: the replay must be kept when a clip fails")


def main():
    if not os.path.exists(TOOL):
        print(f"missing {TOOL}; run ./build.sh first")
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    sample = os.path.join(WORK, "sample.mkv")
    make_sample(sample)
    duration, streams = probe(sample)
    check(abs(duration - SAMPLE_SECONDS) < 0.5 and streams == ["audio", "video"], "sample video")

    test_basic(sample)
    test_keep_and_buffer_clamp(sample)
    test_failures(sample)
    print(f"extraction tests: 4 cases, {checks} checks, {len(failures)} failed")
    if not failures:
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
