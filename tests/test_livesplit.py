"""Runs the real LiveSplit client and gold tracker (build/tests/ls_harness.exe)
against scripted fake LiveSplit servers and checks the golds it reports.

Every server binds 127.0.0.1 on an OS-assigned port; nothing talks to a real
LiveSplit.
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_livesplit import READ_ONLY_COMMANDS, FakeLiveSplit, run_script, seconds_to_ticks  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = os.path.join(ROOT, "build", "tests", "ls_harness.exe")
WALL_TOLERANCE_MS = 150

failures = []
checks = 0


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        failures.append(what)
        print("FAIL:", what)


def parse_events(stdout):
    events = []
    for line in stdout.splitlines():
        if not line.strip():
            continue
        kind, *fields = line.split("\t")
        ev = {"kind": kind}
        for f in fields:
            key, _, value = f.partition("=")
            ev[key] = value
        events.append(ev)
    return events


def run_scenario(name, fake, script):
    duration_ms = int((script[-1][0] + 2.0) * 1000)
    proc = subprocess.Popen([HARNESS, str(fake.port), str(duration_ms)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    try:
        if not fake.connected.wait(5):
            raise RuntimeError(f"{name}: the harness did not connect")
        time.sleep(0.2)  # a few polls see the timer stopped, so the start is observed
        done = run_script(fake, script)
        out, err = proc.communicate(timeout=duration_ms / 1000 + 15)
    finally:
        if proc.poll() is None:
            proc.kill()
        fake.close()
    stdout = out.decode("utf-8")
    stderr = err.decode("utf-8", "replace")
    if os.environ.get("GOLD_CLIPS_TEST_VERBOSE"):
        print(f"--- {name}: {len(fake.commands)} commands\n{stdout}{stderr}")
    check(proc.returncode == 0, f"{name}: harness exit code {proc.returncode}")
    return parse_events(stdout), done, stderr


def action_time(done, index):
    return done[index][0]


def check_commands(name, fake):
    unexpected = [c for c in fake.commands
                  if c not in READ_ONLY_COMMANDS and not (c.startswith("getsplitname ") and c[13:].isdigit())]
    check(fake.commands and not unexpected, f"{name}: unexpected commands sent: {unexpected}")
    check(not fake.bad_commands, f"{name}: commands LiveSplit could not answer: {fake.bad_commands}")


def check_golds(name, events, expected, done):
    golds = [e for e in events if e["kind"] == "GOLD"]
    check(len(golds) == len(expected),
          f"{name}: {len(golds)} golds reported, expected {len(expected)}: {golds}")
    for got, want in zip(golds, expected):
        run, split, seg, best, window, seg_actions = want
        label = f"{name}: gold run {run} split {split}"
        check(got["run"] == str(run), f"{label}: run {got['run']}")
        check(got["split"] == str(split), f"{label}: split {got['split']}")
        check(got["seg"] == str(seconds_to_ticks(seg)), f"{label}: seg {got['seg']}")
        want_best = "-" if best is None else str(seconds_to_ticks(best))
        check(got["best"] == want_best, f"{label}: best {got['best']}, expected {want_best}")
        check(got["window"] == str(window), f"{label}: window {got['window']}")
        if window:
            start_i, end_i = seg_actions
            want_ms = (action_time(done, end_i) - action_time(done, start_i)) * 1000
            got_ms = int(got["wall_ms"])
            check(abs(got_ms - want_ms) <= WALL_TOLERANCE_MS,
                  f"{label}: segment wall time {got_ms} ms, script says {want_ms:.0f} ms")
    return golds


def scenario_known_bests():
    name = "known bests"
    fake = FakeLiveSplit("Ratchet & Clank: Going Commando", "Any% (No QE)",
                         ["Level 1", "Größe 2", "Boss: Phase 3", "Level 4", "End"],
                         [10.0, 15.0, 20.0, 12.0, 8.0])
    script = [
        (0.3, "start"),               # 0  run 1
        (0.8, "split", 9.5),          # 1  9.5 < 10: gold
        (1.3, "split", 25.0),         # 2  15.5 > 15
        (1.8, "split", 44.0),         # 3  19 < 20: gold ...
        (2.1, "undo"),                # 4  ... undone, its clip cancelled
        (2.5, "split", 46.0),         # 5  21 > 20
        (2.9, "skip"),                # 6
        (3.4, "split", 64.0),         # 7  final split after a skip: not checked
        (3.8, "reset"),               # 8  bests become 9.5, 15, 20, 12, 8
        (4.2, "start"),               # 9  run 2
        (4.7, "split", 9.0),          # 10 9 < 9.5: gold
        (5.2, "split_many", [23.0, 42.0]),  # 11 two splits in one poll: not checked
        (5.7, "split", 53.0),         # 12 previous Best Segments total never seen
        (5.8, "pause"),               # 13
        (6.0, "resume"),              # 14
        (6.2, "split", 60.0),         # 15 final split, 7 < 8: gold
        (6.6, "reset"),               # 16
    ]
    events, done, stderr = run_scenario(name, fake, script)
    check_commands(name, fake)

    starts = [e for e in events if e["kind"] == "RUN_START"]
    check(len(starts) == 2, f"{name}: {len(starts)} run starts")
    for s in starts:
        check(s["observed"] == "1", f"{name}: run {s['run']} start not observed")
        check(s["game"] == "Ratchet & Clank: Going Commando", f"{name}: game {s['game']!r}")
        check(s["category"] == "Any% (No QE)", f"{name}: category {s['category']!r}")
        check(s["splits"] == "5", f"{name}: split count {s['splits']}")

    golds = check_golds(name, events, [
        (1, 0, 9.5, 10.0, 1, (0, 1)),
        (1, 2, 19.0, 20.0, 1, (2, 3)),
        (2, 0, 9.0, 9.5, 1, (9, 10)),
        (2, 4, 7.0, 8.0, 1, (12, 15)),
    ], done)
    if len(golds) >= 2:
        check(golds[1]["name"] == "Boss: Phase 3", f"{name}: split name {golds[1]['name']!r}")

    undos = [e for e in events if e["kind"] == "UNDO"]
    check(len(undos) == 1 and undos[0]["from"] == "2" and undos[0]["cancelled"] == "1",
          f"{name}: undo events {undos}")
    ends = [e for e in events if e["kind"] == "RUN_END"]
    check([e["lost"] for e in ends] == ["0", "0"], f"{name}: run ends {ends}")

    queued = [(e["run"], e["split"]) for e in events if e["kind"] == "QUEUED"]
    check(queued == [("1", "0"), ("2", "0"), ("2", "4")], f"{name}: queued clips {queued}")
    check(stderr.count("connected to LiveSplit") == 1, f"{name}: connections in log:\n{stderr}")


def scenario_first_run_and_reconnects():
    name = "first run, drop and stall"
    fake = FakeLiveSplit("Test Game", "Glitchless", ["Größe", "B", "C", "D"],
                         [None, None, None, None], line_end="\r\n", update_bests=False)
    script = [
        (0.3, "start"),          # 0  run 1, no bests yet
        (0.7, "split", 5.0),     # 1  gold (no best)
        (1.1, "split", 11.0),    # 2  gold (no best)
        (1.3, "drop"),           # 3  connection lost; reconnect about 2 s later
        (3.8, "split", 20.0),    # 4  run 2 was picked up mid-run: not checked
        (4.3, "split", 26.0),    # 5  final split: gold (no best)
        (4.8, "reset"),          # 6
        (5.0, "stall", 1.5),     # 7  a response takes too long: reconnect
        (8.8, "start"),          # 8  run 3
        (9.3, "split", 4.0),     # 9  gold (no best)
        (9.6, "wait"),
    ]
    events, done, stderr = run_scenario(name, fake, script)
    check_commands(name, fake)

    check_golds(name, events, [
        (1, 0, 5.0, None, 1, (0, 1)),
        (1, 1, 6.0, None, 1, (1, 2)),
        (2, 3, 6.0, None, 1, (4, 5)),
        (3, 0, 4.0, None, 1, (8, 9)),
    ], done)
    golds = [e for e in events if e["kind"] == "GOLD"]
    if golds:
        check(golds[0]["name"] == "Größe", f"{name}: UTF-8 split name {golds[0]['name']!r}")

    starts = [(e["run"], e["first"], e["observed"]) for e in events if e["kind"] == "RUN_START"]
    check(starts == [("1", "0", "1"), ("2", "2", "0"), ("3", "0", "1")], f"{name}: run starts {starts}")
    ends = [(e["run"], e["lost"]) for e in events if e["kind"] == "RUN_END"]
    check(ends == [("1", "1"), ("2", "0")], f"{name}: run ends {ends}")
    queued = [(e["run"], e["split"]) for e in events if e["kind"] == "QUEUED"]
    check(queued == [("1", "0"), ("1", "1"), ("2", "3"), ("3", "0")], f"{name}: queued clips {queued}")

    check(fake.connections == 3, f"{name}: {fake.connections} connections, expected 3")
    check(stderr.count("lost connection to LiveSplit") == 2, f"{name}: lost-connection logs:\n{stderr}")
    check("no response to" in stderr, f"{name}: the stall was not reported as a timeout:\n{stderr}")


def main():
    if not os.path.exists(HARNESS):
        print(f"missing {HARNESS}; run ./build.sh first")
        return 1
    scenario_known_bests()
    scenario_first_run_and_reconnects()
    print(f"LiveSplit client tests: 2 scenarios, {checks} checks, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
