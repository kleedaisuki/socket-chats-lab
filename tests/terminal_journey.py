"""Exercise the real FTXUI client through a native POSIX PTY, not --plain."""
from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import errno
import os
from pathlib import Path
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time

from client_journey import status
from journey import CREDENTIALS, Process, ROOT, endpoint, require, send_message
from wire import Peer

sys.path.insert(0, str(ROOT / "scripts"))
from terminal_probe import Projection


class Terminal:
    def __init__(self, argv, env, columns=120, rows=32):
        import fcntl
        import pty
        import termios
        self.master, self.slave = pty.openpty()
        self.original = termios.tcgetattr(self.slave)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        self.screen = Projection(columns, rows)
        self.output = bytearray()
        self.query_tail = b""
        self.process = subprocess.Popen(
            argv, env=env, stdin=self.slave, stdout=self.slave, stderr=self.slave,
            start_new_session=True,
            preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))

    def pump(self, timeout=.05):
        if not select.select([self.master], [], [], timeout)[0]:
            return
        try:
            chunk = os.read(self.master, 65536)
        except OSError as error:
            if error.errno != errno.EIO:
                raise
            return
        self.output.extend(chunk)
        self.screen.feed(chunk)
        # Reply to FTXUI's terminal discovery even when a query spans reads.
        query = self.query_tail + chunk
        old = len(self.query_tail)
        for match in re.finditer(rb"\x1b\[(?:6n|c|0c|>c|>0c)", query):
            if match.end() <= old:
                continue
            answer = b"\x1b[1;1R" if match.group().endswith(b"6n") else b"\x1b[?1;2c"
            os.write(self.master, answer)
        self.query_tail = query[-16:]

    def drain(self, seconds=.2):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump(.02)

    def wait(self, predicate, description=None, timeout=12):
        if isinstance(predicate, str):
            needle = predicate
            predicate = lambda text: needle in text
            description = description or needle
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump()
            if predicate(self.screen.text()):
                return self.screen.text()
            if self.process.poll() is not None:
                break
        raise AssertionError(f"screen missing {description}:\n{self.screen.text()}\n"
                             f"exit={self.process.poll()}, unknown VT={self.screen.unknown}")

    def write(self, text):
        os.write(self.master, text.encode("utf-8") if isinstance(text, str) else text)

    def command(self, text):
        self.write(text + "\r")

    def resize(self, columns, rows):
        import fcntl
        import termios
        self.drain()
        self.screen = Projection(columns, rows)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        os.killpg(self.process.pid, signal.SIGWINCH)

    def finish(self, expected=0, interactive=True):
        import termios
        deadline = time.monotonic() + 8
        while self.process.poll() is None and time.monotonic() < deadline:
            self.pump()
        require(self.process.poll() == expected,
                f"terminal exit: expected {expected}, got {self.process.poll()}")
        self.drain(.05)
        require(termios.tcgetattr(self.slave) == self.original,
                "terminal must restore all original termios attributes")
        if interactive:
            require(b"\x1b[?1049h" in self.output and b"\x1b[?1049l" in self.output,
                    "fullscreen client must enter and restore alternate screen")
            require(b"\x1b[?25h" in self.output, "exit must restore cursor visibility")

    def close(self):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait(timeout=5)
        os.close(self.master)
        os.close(self.slave)


def receive_text(peer, expected):
    frame = peer.wait(4)
    require(frame.fields[4] == expected, f"remote text mismatch: {frame.fields[4]!r} != {expected!r}")
    return frame


def publish(peer, text):
    message_id, nonce = send_message(peer, text)
    echo = receive_text(peer, text)
    require(echo.fields[5] == nonce, "peer publication echo nonce")
    return message_id


def main():
    parser = argparse.ArgumentParser()
    for name in ("server", "daemon", "client"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    if os.name != "posix":
        parser.error("this test requires POSIX pty and termios")
    import termios
    for name in ("server", "daemon", "client"):
        setattr(args, name, str(Path(getattr(args, name)).resolve()))
    (ROOT / ".temp").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="terminal-", dir=ROOT / ".temp") as directory:
        work = Path(directory)
        state = work / "alice"
        env = dict(os.environ, CHAT_CREDENTIALS=CREDENTIALS, CHAT_TOKEN="alice-secret",
                   TERM="xterm-256color", COLORTERM="truecolor", LANG="C.UTF-8", TZ="Asia/Shanghai")
        server = Process([args.server, "--memory", "--port", "0"], env, work, "server1")
        daemon = None
        terminals = []
        try:
            port = server.ready()
            daemon = Process([args.daemon, "--port", str(port), "--tenant", "alpha",
                              "--user", "alice", "--state-dir", str(state)], env, work, "daemon")
            ipc = daemon.ready()
            _, secret = endpoint(state)
            with Peer(ipc) as observer, Peer(port) as bob:
                observer.hello("alpha", "alice", secret)
                status(observer, "online")
                bob.hello("alpha", "bob", "bob-secret")
                publish(bob, "CACHE_HISTORY_ONCE")
                seed_frame = observer.wait(4)
                ui = Terminal([args.client, "--state-dir", str(state)], env)
                terminals.append(ui)
                ui.wait("在线")
                ui.wait("CACHE_HISTORY_ONCE")
                ui.drain(.3)
                require(ui.screen.text().count("CACHE_HISTORY_ONCE") == 1,
                        "cache replay and automatic server history must deduplicate")
                modes = termios.tcgetattr(ui.slave)
                require(not modes[3] & (termios.ICANON | termios.ECHO),
                        "FTXUI must enter native noncanonical/no-echo mode")
                require(b"\x1b[" in ui.output, "fullscreen TUI must produce VT output")
                require(observer.rooms()["lobby"][1] == 1, "cache/history display must not auto-read")
                require(re.search(r"bob\s+\d\d:\d\d", ui.screen.text()),
                        "conversation must show author and local clock time")
                expected_clock = datetime.fromtimestamp(
                    int(seed_frame.fields[3]) / 1000, timezone(timedelta(hours=8))).strftime("%H:%M")
                require(expected_clock in ui.screen.text(), "timestamp must use configured local time")
                require("[live]" not in ui.screen.text() and "[online]" not in ui.screen.text(),
                        "interactive conversation must not expose append-log status labels")

                # FTXUI Input must edit Unicode graphemes, not UTF-8 bytes.
                ui.write("你好🙂错")
                ui.write(b"\x7f\x7f")
                ui.write("🌷，PTY 世界\r")
                receive_text(bob, "你好🌷，PTY 世界")
                ui.wait("你好🌷，PTY 世界")
                ui.wait("已发送")
                # Home + Right + Delete + End + Left test insertion within the draft.
                ui.write("甲乙丁")
                ui.write(b"\x1b[H\x1b[C\x1b[3~")
                ui.write("丙")
                ui.write(b"\x1b[F\x1b[D")
                ui.write("乙\r")
                receive_text(bob, "甲丙乙丁")
                ui.wait("甲丙乙丁")
                ui.write("Ae\u0301")
                ui.write(b"\x7f")
                ui.write("B\r")
                receive_text(bob, "AB")

                before = len(ui.output)
                injected = "INJECT_OK 🌙\x1b]0;PTY_OWNED\x07\x1b[31m终点"
                message_id = publish(bob, injected)
                ui.wait("终点")
                raw = bytes(ui.output[before:])
                require(b"\x1b]0;PTY_OWNED" not in raw and b"\x07" not in raw and b"\x1b[31m" not in raw,
                        "peer OSC/BEL/SGR injection must not reach the terminal")
                require(observer.rooms()["lobby"][1] == 2, "viewing live text must not auto-read")
                ui.command("/rooms")
                ui.wait("2 条未读")
                ui.write(b"\x12")  # Ctrl+R, independent peer observes cursor.
                receipt = bob.wait(8)
                require(receipt.fields == ["lobby", "alice", message_id],
                        f"Ctrl+R read cursor differs from visible tail: {receipt.fields}")
                ui.wait(lambda text: "条未读" not in text, "unread cleared")
                require(observer.rooms()["lobby"][1] == 0, "explicit read must clear unread")

                ui.write(b"\x1bOP")  # F1
                ui.wait("键盘，也可以很轻松")
                ui.write(b"\x1b")
                ui.wait(lambda text: "键盘，也可以很轻松" not in text, "Esc dismisses help")
                ui.command("/join study")
                ui.wait("# study")
                ui.write("ROOM_STUDY_ONLY\r")
                study_message = receive_text(bob, "ROOM_STUDY_ONLY")
                require(study_message.fields[1] == "study", "room activation must change send target")
                ui.wait("ROOM_STUDY_ONLY")
                require("INJECT_OK" not in ui.screen.text(), "inactive lobby leaked into study view")
                ui.write(b"\t")
                ui.wait("INJECT_OK")
                require("ROOM_STUDY_ONLY" not in ui.screen.text(), "Tab must switch active conversation only")
                ui.write("LOBBY_DRAFT")
                ui.write(b"\t")
                ui.wait("ROOM_STUDY_ONLY")
                require("LOBBY_DRAFT" not in ui.screen.text(), "per-room draft must not bleed across rooms")
                ui.write(b"\t")
                ui.wait("LOBBY_DRAFT")
                ui.write(b"\x1b[H")
                for _ in "LOBBY_DRAFT":
                    ui.write(b"\x1b[3~")

                for index in range(16):
                    publish(bob, f"SCROLL_{index:02d}")
                ui.wait("SCROLL_15")
                ui.write(b"\x1b[5~")
                ui.wait(lambda text: bool(re.search(r"SCROLL_\d\d", text)) and "SCROLL_15" not in text,
                        "PgUp moves away from live tail")
                ui.write(b"\x12")
                scrolled_receipt = bob.wait(8)
                visible_numbers = [int(value) for value in re.findall(r"SCROLL_(\d\d)", ui.screen.text())]
                require(visible_numbers and max(visible_numbers) < 15,
                        "scrolled read must only acknowledge history actually visible")
                # IDs are consecutive within this isolated server; cross-check actual peer history.
                history = bob.history()
                visible_ids = [int(frame.fields[0]) for frame in history
                               if frame.fields[4] in {f"SCROLL_{number:02d}" for number in visible_numbers}]
                require(int(scrolled_receipt.fields[2]) <= max(visible_ids),
                        "Ctrl+R must not read past the projected visible message maximum")
                ui.write(b"\x1b[6~")
                ui.wait("SCROLL_15")

                ui.resize(40, 12)
                ui.wait("SCROLL_15")
                ui.wait("写消息")
                require(len(ui.screen.cells) == 12 and all(len(row) == 40 for row in ui.screen.cells),
                        "narrow viewport must remain bounded")
                ui.write("窄屏发送\r")
                receive_text(bob, "窄屏发送")
                ui.wait("窄屏发送")
                ui.resize(120, 32)
                ui.wait("窄屏发送")
                ui.wait("房间")

                server.close()
                ui.wait("离线")
                ui.write("离线草稿 🌷")
                ui.write(b"\r")
                ui.wait("草稿已保留")
                ui.wait("离线草稿 🌷")
                server = Process([args.server, "--memory", "--port", str(port)], env, work, "server2")
                server.ready()
                ui.wait("在线", timeout=25)
                ui.wait("离线草稿 🌷")
                with Peer(port) as after:
                    after.hello("alpha", "bob", "bob-secret")
                    require(not after.history(), "offline Enter must not silently queue or resend draft")
                    ui.write(b"\r")
                    receive_text(after, "离线草稿 🌷")
                ui.wait("已发送")
                ui.drain(.3)
                idle_start = len(ui.output)
                ui.drain(.35)
                idle = bytes(ui.output[idle_start:])
                require(not idle, f"idle FTXUI must not redraw ({len(idle)} bytes / 350 ms)")
                ui.write(b"\x03")
                ui.finish()

            for exit_bytes in (b"/quit\r", b"\x04"):
                other = Terminal([args.client, "--state-dir", str(state)], env)
                terminals.append(other)
                other.wait("在线")
                other.write(exit_bytes)
                other.finish()
            broken = Terminal([args.client, "--state-dir", str(work / "missing")], env)
            terminals.append(broken)
            broken.wait("No usable daemon endpoint")
            broken.finish(expected=1, interactive=False)
            print("PASS terminal: real FTXUI PTY projection, Unicode editing, dedup, rooms/drafts, help, scroll-visible explicit read, OSC safety, narrow resize, offline same-UI reconnect, idle and terminal restoration")
        finally:
            for terminal in terminals:
                terminal.close()
            if daemon:
                daemon.close()
            server.close()


if __name__ == "__main__":
    main()
