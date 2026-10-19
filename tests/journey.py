"""Exercise actual server/daemon/TUI processes; no third-party Python modules."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid

from wire import HEADER, Peer, pack

ROOT = Path(__file__).resolve().parents[1]
CREDENTIALS = "alpha:alice:alice-secret;alpha:bob:bob-secret;beta:alice:beta-secret"


class Process:
    def __init__(self, argv: list[str], env: dict[str, str], log_dir: Path, name: str):
        self.log = (log_dir / f"{name}.stderr.log").open("w", encoding="utf-8")
        self.lines: queue.Queue[str] = queue.Queue()
        self.process = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=self.log,
                                        text=True, encoding="utf-8", errors="replace")
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip())

    def ready(self, timeout=20) -> int:
        deadline = time.monotonic() + timeout
        seen = []
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=.2)
                seen.append(line)
                if line.startswith("READY "):
                    return int(line.rsplit(":", 1)[1].split()[0])
            except queue.Empty:
                if self.process.poll() is not None:
                    break
        self.log.flush()
        error = Path(self.log.name).read_text(encoding="utf-8", errors="replace")
        raise RuntimeError(f"process not ready: {seen}, exit={self.process.poll()}, stderr={error}")

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.thread.join(timeout=2)
        self.process.stdout.close()
        self.log.close()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def supports_posix_modes(directory: Path):
    if os.name == "nt":
        return False
    probe = directory / ".permissions-probe"
    try:
        probe.write_text("", encoding="utf-8")
        probe.chmod(0o600)
        supported = probe.stat().st_mode & 0o077 == 0
    finally:
        probe.unlink(missing_ok=True)
    if not supported:
        print("NOTE filesystem ignores POSIX chmod (e.g. WSL DrvFS without metadata); permission-bit checks unavailable", flush=True)
    return supported


def send_message(peer: Peer, text: str, room="lobby", nonce=None):
    nonce = nonce or uuid.uuid4().hex
    request = peer.send(3, [room, nonce, text])
    ack = peer.wait(15, request)
    return ack.fields[0], nonce


def direct_journey(port: int):
    with Peer(port) as alice, Peer(port) as bob, Peer(port) as outsider:
        alice.hello("alpha", "alice", "alice-secret", fragments=True)
        bob.hello("alpha", "bob", "bob-secret")
        outsider.hello("beta", "alice", "beta-secret")
        require(alice.rooms()["lobby"] == (0, 0), "initial room state")
        text = "你好，Klee！🌸 café e\u0301 — streaming chat\n第二行"
        message_id, nonce = send_message(alice, text)
        event = bob.wait(4, 0)
        require(event.fields == [message_id, "lobby", "alice", event.fields[3], text, nonce], "message metadata")
        require(abs(int(event.fields[3]) - int(time.time() * 1000)) < 10000, "UTC millisecond timestamp")
        require(bob.rooms()["lobby"][1] == 1, "other user's unread")
        require(alice.rooms()["lobby"][1] == 0, "own messages excluded from unread")
        require(outsider.history() == [], "tenant history isolation")
        require(outsider.rooms()["lobby"] == (0, 0), "tenant metadata isolation")
        same_id, _ = send_message(alice, text, nonce=nonce)
        require(same_id == message_id and len(alice.history()) == 1, "idempotent retry")
        request = alice.send(3, ["lobby", nonce, "different payload"])
        require(alice.wait(11, request).fields[0] == "conflict", "nonce payload conflict")
        request = bob.send(7, ["lobby", message_id])
        receipt = bob.wait(8)
        require(receipt.fields == ["lobby", "bob", message_id], "read receipt")
        require(bob.rooms()["lobby"][1] == 0, "read clears unread")
        bob.send(7, ["lobby", "0"])
        require(bob.wait(8).fields[2] == message_id, "read cursor must never move backward")
        request = alice.send(14, ["research"])
        alice.wait(15, request)
        send_message(bob, "room-scoped", "research")
        require(len(alice.history("research")) == 1, "joined public room history")
        require("research" not in outsider.rooms(), "tenant room isolation")
        long_text = "带宽优化 compression repeated text 🌸 " * 1000
        long_id, _ = send_message(alice, long_text)
        messages = bob.history(after=message_id)
        require(len(messages) == 1 and messages[0].fields[0] == long_id, "history cursor")
        require(messages[0].fields[4] == long_text and messages[0].compressed, "compressed UTF-8 round trip")
        require(len(alice.history(limit=1)) == 1, "bounded history page")
        # A whole batch in one write exercises TCP coalescing and per-session FIFO.
        ids = list(range(10000, 10020))
        alice.socket.sendall(b"".join(pack(9, rid, []) for rid in ids))
        for rid in ids:
            alice.wait(10, rid)
    with Peer(port) as bad:
        rid = bad.send(1, ["alpha", "alice", "wrong-token"])
        require(bad.wait(11, rid).fields[0] == "auth", "authentication rejection")
    with Peer(port) as bad:
        rid = bad.send(3, ["lobby", "anon", "forbidden"])
        require(bad.wait(11, rid).fields[0] == "auth", "pre-auth command rejection")
    with Peer(port) as bad:
        bad.socket.sendall(HEADER.pack(b"SCL1", 1, 9, 0, 1, (1 << 20) + 1, 2))
        try:
            require(bad.socket.recv(1) == b"", "oversized frame should disconnect")
        except (ConnectionResetError, ConnectionAbortedError):
            pass
    print("PASS direct: auth, fragmentation/coalescing, UTF-8, compression, idempotency, tenant isolation, read/unread", flush=True)


def concurrency_journey(port: int):
    def session(index):
        with Peer(port) as peer:
            peer.hello("beta", "alice", "beta-secret")
            for number in range(5):
                send_message(peer, f"parallel-{index}-{number}")
    start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as workers:
        list(workers.map(session, range(16)))
    elapsed = time.monotonic() - start
    with Peer(port) as peer:
        peer.hello("beta", "alice", "beta-secret")
        history = peer.history()
        require(len(history) == 80, "concurrent writes lost or duplicated")
        ids = [int(frame.fields[0]) for frame in history]
        require(ids == sorted(set(ids)), "history must be strictly ordered")
    print(f"PASS concurrency: 16 connections, 80 persisted messages in {elapsed:.3f}s (not a capacity benchmark)", flush=True)


def endpoint(state: Path):
    lines = (state / "endpoint").read_text(encoding="utf-8").splitlines()
    return int(lines[0]), lines[1]


def wait_online(peer: Peer):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        status = peer.wait(16, timeout=deadline - time.monotonic())
        if status.fields and status.fields[0] == "online":
            return
    raise TimeoutError("daemon did not authenticate upstream")


def daemon_journey(args, port: int, env, work: Path):
    state = work / "alice"
    daemon_env = dict(env, CHAT_TOKEN="alice-secret")
    process = Process([args.daemon, "--host", "127.0.0.1", "--port", str(port),
                       "--tenant", "alpha", "--user", "alice", "--state-dir", str(state)],
                      daemon_env, work, "daemon")
    try:
        ipc_port = process.ready()
        stored_port, token = endpoint(state)
        require(ipc_port == stored_port, "daemon discovery endpoint")
        with Peer(ipc_port) as ui1, Peer(ipc_port) as ui2, Peer(port) as bob:
            ui1.hello("alpha", "alice", token)
            ui2.hello("alpha", "alice", token)
            wait_online(ui1)
            wait_online(ui2)
            bob.hello("alpha", "bob", "bob-secret")
            # The first request IDs on both UI sockets intentionally collide.
            rid1 = ui1.send(9, [])
            rid2 = ui2.send(12, [])
            ui1.wait(10, rid1)
            ui2.wait(13, rid2)
            text = "daemon IPC 实际旅程 🌷"
            message_id, _ = send_message(ui1, text)
            require(bob.wait(4, 0).fields[4] == text, "daemon outbound message")
            inbound = "incoming 中文 🐈"
            send_message(bob, inbound)
            require(ui2.wait(4, 0).fields[4] in (text, inbound), "daemon local fanout")
            history = ui1.history()
            require(any(frame.fields[0] == message_id for frame in history), "daemon history proxy")
        # Real CLI process, not just the protocol test peer.
        cli = subprocess.Popen([args.client, "--state-dir", str(state), "--plain"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", env=env)
        try:
            cli.stdin.write("/rooms\n/history\n")
            cli.stdin.flush()
            time.sleep(1)
            output, _ = cli.communicate("/quit\n", timeout=10)
        except BaseException:
            cli.kill()
            cli.communicate()
            raise
        (work / "tui-plain.txt").write_text(output, encoding="utf-8")
        require(cli.returncode == 0, f"TUI exit {cli.returncode}: {output}")
        require("\x1b" not in output and "lobby" in output, f"plain TUI must be readable: {output}")
        require("daemon IPC 实际旅程" in output, f"TUI should show UTF-8 history: {output}")
        cache = state / "events.jsonl"
        deadline = time.monotonic() + 3
        while (not cache.exists() or not cache.stat().st_size) and time.monotonic() < deadline:
            time.sleep(.05)
        lines = cache.read_text(encoding="utf-8").splitlines()
        require(bool(lines), "daemon cache must persist events")
        for line in lines:
            json.loads(line)
        require("alice-secret" not in cache.read_text(encoding="utf-8"), "cache leaks auth token")
        if supports_posix_modes(work):
            require(state.stat().st_mode & 0o077 == 0, "private daemon state directory")
            require((state / "endpoint").stat().st_mode & 0o077 == 0, "private IPC secret")
        with Peer(ipc_port) as attacker:
            rid = attacker.send(1, ["alpha", "alice", "invalid-ipc-token"])
            require(attacker.wait(11, rid).fields[0] == "auth", "local IPC authorization")
        print("PASS daemon/TUI: private IPC, colliding request IDs, UTF-8, cache, real piped user commands", flush=True)
    finally:
        process.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--daemon", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--postgres", action="store_true", help="CHAT_DB_DSN must point at a fresh test database")
    args = parser.parse_args()
    for key in ("server", "daemon", "client"):
        setattr(args, key, str(Path(getattr(args, key)).resolve()))
    temp = ROOT / ".temp"
    temp.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="journey-", dir=temp) as directory:
        work = Path(directory)
        env = dict(os.environ, CHAT_CREDENTIALS=CREDENTIALS)
        argv = [args.server, "--port", "0", "--threads", "4"]
        if not args.postgres:
            argv.append("--memory")
        server = Process(argv, env, work, "server")
        try:
            port = server.ready()
            direct_journey(port)
            concurrency_journey(port)
            daemon_journey(args, port, env, work)
            if args.postgres:
                with Peer(port) as peer:
                    peer.hello("alpha", "alice", "alice-secret")
                    expected_history = [frame.fields for frame in peer.history()]
                with Peer(port) as peer:
                    peer.hello("alpha", "bob", "bob-secret")
                    expected_rooms = peer.rooms()
                server.close()
                server = Process(argv, env, work, "server-restart")
                port = server.ready()
                with Peer(port) as peer:
                    peer.hello("alpha", "alice", "alice-secret")
                    require([frame.fields for frame in peer.history()] == expected_history,
                            "PostgreSQL history and metadata must survive server restart exactly")
                with Peer(port) as peer:
                    peer.hello("alpha", "bob", "bob-secret")
                    require(peer.rooms() == expected_rooms, "read/unread and room metadata must persist exactly")
                print("PASS PostgreSQL: messages and read cursors survive server restart", flush=True)
        except BaseException:
            for log in work.glob("*.log"):
                print(f"--- {log.name} ---\n{log.read_text(encoding='utf-8', errors='replace')}", file=sys.stderr)
            raise
        finally:
            server.close()
    print("ALL USER JOURNEYS PASSED", flush=True)


if __name__ == "__main__":
    main()
