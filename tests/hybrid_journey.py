"""Manual Windows TUI/daemon -> WSL PostgreSQL journey; leaves the stack running."""
from __future__ import annotations

from contextlib import ExitStack
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import hybrid
from journey import endpoint, require
from wire import Peer


class Tui:
    """Keep a real native CLI attached while recording asynchronous output."""
    def __init__(self, user, log_dir, name=None):
        self.log = (log_dir / f"{name or user}.tui.log").open("w", encoding="utf-8")
        self.lines = []
        self.changed = threading.Condition()
        self.process = subprocess.Popen(
            [str(hybrid.client_path()), "--state-dir", str(hybrid.daemon_state(user)), "--plain"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="strict", bufsize=1)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            with self.changed:
                self.lines.append(line.rstrip("\n"))
                self.log.write(line)
                self.log.flush()
                self.changed.notify_all()

    def command(self, text):
        mark = len(self.lines)
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()
        return mark

    def wait(self, text, after=0, timeout=20):
        deadline = time.monotonic() + timeout
        with self.changed:
            while True:
                for line in self.lines[after:]:
                    if text in line:
                        return line
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.process.poll() is not None:
                    raise AssertionError(f"TUI missing {text!r}: {self.lines[after:]}")
                self.changed.wait(min(remaining, .25))

    def close(self):
        if self.process.poll() is None:
            self.command("/quit")
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.reader.join(timeout=3)
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()
        require(self.process.returncode == 0, "native TUI did not exit cleanly")


def frame_matching(peer, kind, predicate, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = peer.wait(kind, timeout=deadline - time.monotonic())
        if predicate(frame.fields):
            return frame
    raise AssertionError(f"No matching frame kind {kind}")


def status(peer, name):
    return frame_matching(peer, 16, lambda fields: fields[0] == name)


def attach(user, config):
    port, token = endpoint(hybrid.daemon_state(user))
    peer = Peer(port)
    try:
        peer.hello(config["tenant"], user, token)
        status(peer, "online")
        return peer
    except BaseException:
        peer.close()
        raise


def main():
    require(os.name == "nt", "Run with native Windows Python, not WSL Python")
    config = hybrid.load_config()
    work = hybrid.STATE / "verification"
    work.mkdir(parents=True, exist_ok=True)
    report = {"started_utc": datetime.now(timezone.utc).isoformat(),
              "room": "check_" + uuid.uuid4().hex[:12], "checks": [], "passed": False}
    room = report["room"]
    before = hybrid.read_runtime()
    backend_stopped = False

    def passed(name, **values):
        report["checks"].append({"name": name, **values})
        print("PASS " + name, flush=True)

    try:
        with ExitStack() as cleanup:
            alice = cleanup.enter_context(attach("alice", config))
            bob = cleanup.enter_context(attach("bob", config))
            a = Tui("alice", work)
            cleanup.callback(a.close)
            b = Tui("bob", work)
            cleanup.callback(b.close)
            for ui in (a, b):
                ui.wait("[online]")
                mark = ui.command("/join " + room)
                ui.wait("[joined] #" + room, after=mark)
            passed("two Windows native TUI processes attached and joined", alice_pid=a.process.pid,
                   bob_pid=b.process.pid)

            first = f"Windows 发给 WSL：你好，Klee 🌸 café e\u0301 [{room}]"
            second = f"WSL 经 Windows 返回：收到啦 🐈 中文双向验证 [{room}]"
            mark = b.command("/rooms")
            b.wait(room, after=mark)
            a.command(first)
            event = frame_matching(bob, 4, lambda f: f[1] == room and f[4] == first)
            first_id = event.fields[0]
            require(event.fields[2] == "alice", "wrong first sender")
            require(abs(int(event.fields[3]) - int(time.time() * 1000)) < 15000,
                    "server UTC millisecond timestamp missing or invalid")
            line = b.wait(first)
            require("[live]" in line and "#" + room in line, "TUI lacks live/room metadata")
            require(bob.rooms()[room][1] == 1, "displaying must not automatically read")
            require(alice.rooms()[room][1] == 0, "own message counted unread")
            b.command("/read")
            receipt = frame_matching(alice, 8, lambda f: f == [room, "bob", first_id])
            a.wait(f"[read] #{room} bob through {first_id}")
            require(bob.rooms()[room][1] == 0, "explicit TUI /read did not clear unread")
            passed("Alice TUI -> WSL -> Bob TUI UTF-8, timestamps, unread 1 -> explicit read 0",
                   id=first_id, timestamp_ms=int(event.fields[3]), receipt=receipt.fields)

            b.command(second)
            event = frame_matching(alice, 4, lambda f: f[1] == room and f[4] == second)
            second_id = event.fields[0]
            a.wait(second)
            require(event.fields[2] == "bob", "wrong second sender")
            require(alice.rooms()[room][1] == 1, "reverse message must remain unread")
            expected = [(first_id, first), (second_id, second)]
            history = alice.history(room)
            require([(f.fields[0], f.fields[4]) for f in history] == expected,
                    "online PostgreSQL history differs from TUI messages")
            passed("Bob TUI -> WSL -> Alice TUI; PostgreSQL history exact", id=second_id)

            hybrid.stop_backend()
            backend_stopped = True
            status(alice, "offline")
            status(bob, "offline")
            a.wait("[offline]")
            b.wait("[offline]")
            with ExitStack() as offline_cleanup:
                cached = Tui("alice", work, name="alice-offline")
                offline_cleanup.callback(cached.close)
                cached.wait("[offline]")
                require("[cached]" in cached.wait(second), "offline replay not labeled cached")
                mark = cached.command("/rooms")
                cached.wait("[error] offline", after=mark)
            mark = a.command("离线不应排队发送 🚫")
            a.wait("[error] offline", after=mark)
            passed("same live TUIs/IPC sockets survive outage; cached UTF-8 labeled offline; sends rejected")

            hybrid.start_backend(config)
            backend_stopped = False
            status(alice, "online")
            status(bob, "online")
            a.wait("[online]", after=mark)
            require(alice.rooms()[room][1] == 1, "restart/cache replay silently cleared unread")
            require(bob.rooms()[room][1] == 0, "read cursor did not persist")
            with Peer(config["port"]) as direct:
                direct.hello(config["tenant"], "alice", config["users"]["alice"])
                restored = direct.history(room)
                require([(f.fields[0], f.fields[4]) for f in restored] == expected,
                        "fresh server did not restore exact PostgreSQL history")
            mark = a.command("/history")
            a.wait("[history complete] " + room, after=mark)
            a.wait("[history] #" + room, after=mark)
            a.command("/read")
            frame_matching(bob, 8, lambda f: f == [room, "alice", second_id])
            require(alice.rooms()[room][1] == 0, "post-reconnect TUI read failed")
            passed("server restart restores PostgreSQL messages/read cursors; same IPC reconnects")

            third = "重连后继续发送 🌷 " + room
            b.command(third)
            event = frame_matching(alice, 4, lambda f: f[1] == room and f[4] == third)
            a.wait(third)
            require(alice.rooms()[room][1] == 1, "post-reconnect new message unread missing")
            a.command("/read")
            frame_matching(bob, 8, lambda f: f == [room, "alice", event.fields[0]])
            require(alice.rooms()[room][1] == 0, "post-reconnect second read failed")
            expected.append((event.fields[0], third))
            require([(f.fields[0], f.fields[4]) for f in alice.history(room)] == expected,
                    "reconnect duplicated or lost messages, or queued rejected offline send")
            after = hybrid.read_runtime()
            for user in ("alice", "bob"):
                require(before[user]["pid"] == after[user]["pid"], "daemon restarted during reconnect")
                require((a if user == "alice" else b).process.poll() is None,
                        "original TUI did not survive outage")
            passed("post-reconnect TUI delivery/read and exact history; daemon/TUI PIDs unchanged",
                   daemon_pids={u: after[u]["pid"] for u in ("alice", "bob")}, messages=expected)
        report["passed"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        if backend_stopped:
            hybrid.start_backend(config)
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        (work / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                          encoding="utf-8")


if __name__ == "__main__":
    main()
