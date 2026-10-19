"""Real POSIX PTY user journey; stdlib only, no terminal emulator dependency."""
from __future__ import annotations

import argparse
import errno
import os
from pathlib import Path
import re
import select
import subprocess
import tempfile
import time

from client_journey import status
from journey import CREDENTIALS, Process, ROOT, endpoint, require, send_message
from wire import Peer


class Terminal:
    def __init__(self, argv, env):
        import pty
        import termios
        self.master, self.slave = pty.openpty()
        self.original = termios.tcgetattr(self.slave)
        self.output = bytearray()
        self.process = subprocess.Popen(argv, env=env, stdin=self.slave,
                                        stdout=self.slave, stderr=self.slave)

    def pump(self, timeout=.1):
        if not select.select([self.master], [], [], timeout)[0]:
            return
        try:
            self.output.extend(os.read(self.master, 65536))
        except OSError as error:
            if error.errno != errno.EIO:
                raise

    def wait(self, needle, start=0, timeout=10):
        needle = needle.encode("utf-8")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if needle in self.output[start:]:
                return
            self.pump()
        raise AssertionError(f"terminal missing {needle!r}: {bytes(self.output[start:])!r}")

    def write(self, text):
        os.write(self.master, text.encode("utf-8") if isinstance(text, str) else text)

    def finish(self, expected=0):
        import termios
        deadline = time.monotonic() + 8
        while self.process.poll() is None and time.monotonic() < deadline:
            self.pump()
        require(self.process.poll() == expected,
                f"terminal exit: expected {expected}, got {self.process.poll()}")
        self.pump(0)
        require(termios.tcgetattr(self.slave) == self.original,
                "terminal must restore all original termios attributes")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        os.close(self.master)
        os.close(self.slave)


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
        env = dict(os.environ, CHAT_CREDENTIALS=CREDENTIALS,
                   CHAT_TOKEN="alice-secret", TERM="xterm-256color")
        server = Process([args.server, "--memory", "--port", "0"], env, work, "server")
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
                ui = Terminal([args.client, "--state-dir", str(state)], env)
                terminals.append(ui)
                ui.wait("[online]")
                modes = termios.tcgetattr(ui.slave)
                require(not modes[3] & (termios.ICANON | termios.ECHO | termios.ISIG),
                        "interactive TUI must actually enter native noncanonical mode")
                require(b"\x1b[36m" in ui.output, "interactive prompt must use color")

                # Delete a four-byte emoji and a three-byte CJK code point, then send.
                ui.write("你好🙂错")
                ui.write(b"\x7f\x7f")
                ui.write("🌷，PTY 世界\r")
                sent = bob.wait(4)
                require(sent.fields[4] == "你好🌷，PTY 世界", "whole-codepoint backspace corrupted message")
                ui.wait("[sent]")
                ui.wait("[live] #lobby alice")
                require(" UTC]".encode() in ui.output, "message timestamp must be explicitly UTC")

                injected = "对端消息 🌙\x1b]0;PTY_OWNED\x07\x1b[31m终点"
                message_id, _ = send_message(bob, injected)
                ui.wait("终点")
                require(observer.rooms()["lobby"][1] == 1, "displaying must not implicitly mark read")
                mark = len(ui.output)
                ui.write("/rooms\r")
                ui.wait("UNREAD", mark)
                ui.wait(message_id + "    1", mark)
                plain = re.sub(rb"\x1b\[[0-9;]*[mK]", b"", bytes(ui.output[mark:]))
                require(re.search(rb"lobby +" + message_id.encode() + rb" +1\r?\n", plain),
                        f"/rooms must show latest id and one unread: {plain!r}")
                ui.write("/read\r")
                receipt = bob.wait(8)
                require(receipt.fields == ["lobby", "alice", message_id], "peer must receive explicit TUI read cursor")
                ui.wait(f"[read] #lobby alice through {message_id}")
                require(observer.rooms()["lobby"][1] == 0, "explicit read must clear account unread")
                require(b"\x1b]" not in ui.output and b"\x07" not in ui.output,
                        "untrusted OSC/BEL must not reach terminal")
                # Only application-generated SGR and erase-line escapes are allowed.
                remainder = re.sub(rb"\x1b\[[0-9;]*[mK]", b"", bytes(ui.output))
                require(b"\x1b" not in remainder, "unexpected terminal control escape")
                settle = time.monotonic() + .2
                while time.monotonic() < settle:
                    ui.pump(.02)
                idle_start = len(ui.output)
                idle_end = time.monotonic() + .3
                while time.monotonic() < idle_end:
                    ui.pump(.02)
                idle = bytes(ui.output[idle_start:])
                require(not idle, f"idle terminal must not repeatedly redraw ({len(idle)} bytes in 300 ms)")
                ui.write(b"\x03")
                ui.finish()

                for exit_bytes in (b"/quit\r", b"\x04"):
                    other = Terminal([args.client, "--state-dir", str(state)], env)
                    terminals.append(other)
                    other.wait("[online]")
                    other.write(exit_bytes)
                    other.finish()

                # Terminal is configured before endpoint loading; exceptions must restore it too.
                broken = Terminal([args.client, "--state-dir", str(work / "missing")], env)
                terminals.append(broken)
                broken.wait("No usable daemon endpoint")
                broken.finish(expected=1)
            print("PASS terminal: native PTY, color/UTC metadata, UTF-8 send/backspace, rooms, explicit peer read receipt, OSC safety, Ctrl-C/Ctrl-D/quit and exception termios restoration")
        finally:
            for terminal in terminals:
                terminal.close()
            if daemon:
                daemon.close()
            server.close()


if __name__ == "__main__":
    main()
