"""A scriptable stand-in for LiveSplit's TCP server (standard library only).

It listens on 127.0.0.1 on a port the OS picks, answers the read-only
commands the plugin uses, and records every command it receives so a test can
check that nothing else was sent. Script methods (start, split, undo, ...)
change the timer state the way LiveSplit would.
"""

import socket
import threading
import time

REAL_LIVESPLIT_PORT = 16834

READ_ONLY_COMMANDS = {
    "getcurrenttimerphase",
    "getsplitindex",
    "getsplitcount",
    "getgamename",
    "getcategoryname",
    "getlastsplittime",
    "getcomparisonsplittime Best Segments",
}

TICKS_PER_SECOND = 10_000_000


def format_time(seconds):
    """C# TimeSpan constant format, [-][d.]hh:mm:ss[.fffffff]; None -> "-"."""
    if seconds is None:
        return "-"
    ticks = round(seconds * TICKS_PER_SECOND)
    sign = "-" if ticks < 0 else ""
    total, frac = divmod(abs(ticks), TICKS_PER_SECOND)
    days, rest = divmod(total, 86400)
    hours, rest = divmod(rest, 3600)
    minutes, secs = divmod(rest, 60)
    text = f"{hours:02}:{minutes:02}:{secs:02}"
    if days:
        text = f"{days}.{text}"
    if frac:
        text += f".{frac:07}"
    return sign + text


def seconds_to_ticks(seconds):
    return round(seconds * TICKS_PER_SECOND)


class FakeLiveSplit:
    def __init__(self, game, category, names, best_segments, line_end="\n", update_bests=True):
        self.game = game
        self.category = category
        self.names = list(names)
        self.best = list(best_segments)  # best time per segment, None = none yet
        self.line_end = line_end
        self.update_bests = update_bests

        self.lock = threading.Lock()
        self.phase = "NotRunning"
        self.index = -1
        self.times = [None] * len(self.names)
        self.commands = []
        self.bad_commands = []
        self.stall_next = 0.0
        self.connections = 0
        self.connected = threading.Event()
        self.client = None
        self.closed = False

        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.bind(("127.0.0.1", 0))  # loopback only, OS-assigned port
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        if self.port == REAL_LIVESPLIT_PORT:
            raise RuntimeError("got LiveSplit's real port; refusing to run")
        self.thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.thread.start()

    # -- server ---------------------------------------------------------

    def _accept_loop(self):
        while not self.closed:
            try:
                conn, _ = self.server.accept()
            except OSError:
                return
            with self.lock:
                self.client = conn
                self.connections += 1
            self.connected.set()
            self._serve(conn)
            with self.lock:
                if self.client is conn:
                    self.client = None
            try:
                conn.close()
            except OSError:
                pass

    def _serve(self, conn):
        buf = b""
        while True:
            try:
                data = conn.recv(4096)
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.rstrip(b"\r").decode("utf-8")
                response = self._respond(cmd)
                with self.lock:
                    stall, self.stall_next = self.stall_next, 0.0
                if stall:
                    time.sleep(stall)
                if response is None:
                    continue  # LiveSplit sends nothing for commands it cannot answer
                try:
                    conn.sendall((response + self.line_end).encode("utf-8"))
                except OSError:
                    return

    def _cumulative_best(self, i):
        if any(b is None for b in self.best[: i + 1]):
            return None
        return sum(self.best[: i + 1])

    def _respond(self, cmd):
        with self.lock:
            self.commands.append(cmd)
            n = len(self.names)
            if cmd == "getcurrenttimerphase":
                return self.phase
            if cmd == "getsplitindex":
                return str(self.index)
            if cmd == "getsplitcount":
                return str(n)
            if cmd == "getgamename":
                return self.game
            if cmd == "getcategoryname":
                return self.category
            if cmd == "getlastsplittime":
                return format_time(self.times[self.index - 1]) if self.index > 0 else "-"
            if cmd == "getcomparisonsplittime Best Segments":
                if not 0 <= self.index < n:
                    self.bad_commands.append(cmd + " (no current split)")
                    return None
                return format_time(self._cumulative_best(self.index))
            if cmd.startswith("getsplitname "):
                arg = cmd.split(" ", 1)[1]
                if arg.isdigit() and int(arg) < n:
                    return self.names[int(arg)]
            self.bad_commands.append(cmd)
            return None

    def close(self):
        self.closed = True
        self.drop()
        self.server.close()

    # -- script actions -------------------------------------------------

    def start(self):
        with self.lock:
            self.phase = "Running"
            self.index = 0
            self.times = [None] * len(self.names)

    def _split(self, t):
        self.times[self.index] = t
        self.index += 1
        if self.index == len(self.names):
            self.phase = "Ended"

    def split(self, t):
        with self.lock:
            self._split(t)

    def split_many(self, ts):
        """Several splits at once, as if they happened within one poll."""
        with self.lock:
            for t in ts:
                self._split(t)

    def skip(self):
        self.split(None)

    def undo(self):
        with self.lock:
            self.index -= 1
            self.times[self.index] = None
            self.phase = "Running"

    def pause(self):
        with self.lock:
            self.phase = "Paused"

    def resume(self):
        with self.lock:
            self.phase = "Running"

    def reset(self):
        with self.lock:
            if self.update_bests:
                prev = 0.0
                for i, t in enumerate(self.times):
                    if t is not None and prev is not None:
                        seg = t - prev
                        if self.best[i] is None or seg < self.best[i]:
                            self.best[i] = seg
                    prev = t
            self.phase = "NotRunning"
            self.index = -1

    def drop(self):
        """Close the client connection (LiveSplit's server went away)."""
        with self.lock:
            conn = self.client
        if conn:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            conn.close()

    def stall(self, seconds):
        """Answer the next command only after this many seconds."""
        with self.lock:
            self.stall_next = seconds

    def wait(self):
        pass


def run_script(fake, script):
    """Runs [(seconds, action, *args)] on schedule; returns the actual times."""
    t0 = time.perf_counter()
    done = []
    for at, action, *args in script:
        delay = t0 + at - time.perf_counter()
        if delay > 0:
            time.sleep(delay)
        getattr(fake, action)(*args)
        done.append((time.perf_counter() - t0, action, args))
    return done
