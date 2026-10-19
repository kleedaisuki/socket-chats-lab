"""Cross-platform build, test, and local two-user chat launcher."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def run(argv, **kwargs):
    subprocess.run([str(part) for part in argv], cwd=ROOT, check=True, **kwargs)


def executable(build: Path, name: str):
    filename = name + (".exe" if os.name == "nt" else "")
    candidates = [build / "src" / ("server" if name == "chat-server" else "client") / filename,
                  build / "src" / ("server" if name == "chat-server" else "client") / "Debug" / filename]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"{filename} not built; run python scripts/dev.py build --portable")


class Child:
    def __init__(self, argv, env, label):
        self.label = label
        self.lines: queue.Queue[str] = queue.Queue()
        self.process = subprocess.Popen([str(part) for part in argv], cwd=ROOT, env=env,
                                        stdout=subprocess.PIPE, stderr=None, text=True, encoding="utf-8")
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip())

    def ready(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=.2)
            except queue.Empty:
                if self.process.poll() is not None:
                    raise RuntimeError(f"{self.label} exited with {self.process.returncode}")
                continue
            if line.startswith("READY "):
                return int(line.rsplit(":", 1)[1].split()[0])
        raise TimeoutError(f"{self.label} did not become ready")

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process.stdout.close()


def demo(build: Path, portable: bool):
    state = ROOT / ".cache" / "demo"
    env = dict(os.environ, CHAT_CREDENTIALS="lab:alice:local-alice;lab:bob:local-bob")
    server_args = [executable(build, "chat-server"), "--port", "0"]
    if portable:
        server_args.append("--memory")
    children = []
    try:
        server = Child(server_args, env, "server")
        children.append(server)
        port = server.ready()
        for user in ("alice", "bob"):
            child = Child([executable(build, "chat-daemon"), "--port", port, "--tenant", "lab",
                           "--user", user, "--state-dir", state / user],
                          dict(env, CHAT_TOKEN=f"local-{user}"), f"{user} daemon")
            children.append(child)
            child.ready()
        client = executable(build, "chat")
        print("Two daemons are running. Open a second terminal for Bob:", flush=True)
        print(subprocess.list2cmdline([str(client), "--state-dir", str(state / "bob")]), flush=True)
        print("Alice starts here. /help for commands; /quit stops this demo.", flush=True)
        run([client, "--state-dir", state / "alice"], env=env)
    finally:
        for child in reversed(children):
            child.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["build", "test", "demo"])
    parser.add_argument("--portable", action="store_true", help="Explicit nonpersistent memory server, no libpq required")
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    build = (args.build_dir or ROOT / ".cache" / "build" / ("portable" if args.portable else "dev")).resolve()
    if not build.is_relative_to(ROOT):
        parser.error("build directory must remain inside this repository")
    if args.command == "build":
        run(["cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug",
             f"-DCHAT_WITH_POSTGRES={'OFF' if args.portable else 'ON'}"])
        run(["cmake", "--build", build, "--parallel"])
    elif args.command == "test":
        run(["ctest", "--test-dir", build, "--output-on-failure"])
    else:
        demo(build, args.portable)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"dev: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
