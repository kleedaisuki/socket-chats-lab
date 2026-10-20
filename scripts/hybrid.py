"""Run native Windows clients against a persistent WSL/PostgreSQL backend.

All state stays in .cache/hybrid. Stopping this stack never stops PostgreSQL,
drops a database, or signals an unrelated process. Requires existing local builds.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
STATE = ROOT / ".cache" / "hybrid"
USERS = ("alice", "bob")
PROFILE = "hybrid"


def select_profile(profile):
    global PROFILE, STATE
    if profile not in ("hybrid", "demo"):
        raise ValueError("Unknown deployment profile")
    PROFILE = profile
    STATE = ROOT / ".cache" / ("showcase" if profile == "demo" else "hybrid")


def backend_command(command):
    return ["python3", "scripts/hybrid.py", command, "--profile", PROFILE]


def read_json(path: Path, default=None):
    if not path.exists():
        return default
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def secure_directory(path: Path):
    path.mkdir(parents=True, exist_ok=True)
    if path.is_symlink() or path.resolve() != path.absolute():
        raise RuntimeError("Private hybrid state must not be a link or junction")
    if os.name != "nt":
        raise RuntimeError("Start the hybrid deployment from Windows Python")
    from ctypes import wintypes
    security = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    security.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                         ctypes.POINTER(wintypes.HANDLE)]
    security.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                            wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    security.ConvertSidToStringSidW.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    security.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    security.SetFileSecurityW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_void_p]
    token = wintypes.HANDLE()
    if not security.OpenProcessToken(kernel.GetCurrentProcess(), 8, ctypes.byref(token)):
        raise ctypes.WinError(ctypes.get_last_error())
    size = wintypes.DWORD()
    security.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
    info = ctypes.create_string_buffer(size.value)
    try:
        if not security.GetTokenInformation(token, 1, info, size, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(token)
    sid = ctypes.cast(info, ctypes.POINTER(ctypes.c_void_p))[0]
    text = ctypes.c_void_p()
    descriptor = ctypes.c_void_p()
    try:
        if not security.ConvertSidToStringSidW(sid, ctypes.byref(text)):
            raise ctypes.WinError(ctypes.get_last_error())
        sddl = f"D:P(A;OICI;FA;;;{ctypes.wstring_at(text)})"
        if not security.ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, 1, ctypes.byref(descriptor), None):
            raise ctypes.WinError(ctypes.get_last_error())
        if not security.SetFileSecurityW(str(path), 4 | 0x80000000, descriptor):
            raise ctypes.WinError(ctypes.get_last_error())
        # Repair pre-existing known configuration files as well as inheritance.
        for name in ("config.json", "runtime.json", "server-pid.json"):
            file = path / name
            if file.is_symlink():
                raise RuntimeError("Refusing linked hybrid configuration")
            if file.exists() and not security.SetFileSecurityW(str(file), 4 | 0x80000000, descriptor):
                raise ctypes.WinError(ctypes.get_last_error())
    finally:
        if text.value:
            kernel.LocalFree(text)
        if descriptor.value:
            kernel.LocalFree(descriptor)


def load_config():
    config = read_json(STATE / "config.json")
    if not config:
        raise RuntimeError("Run python scripts/hybrid.py start first")
    return config


def read_runtime():
    return read_json(STATE / "runtime.json", {})


def daemon_state(user):
    if user not in USERS:
        raise ValueError("Unknown hybrid account")
    return STATE / user


def client_path():
    if PROFILE == "demo":
        return ROOT / ".cache/build/dev/src/client/chat.exe"
    return ROOT / ".cache" / "build" / "msvc" / "src" / "client" / "Debug" / "chat.exe"


def daemon_path():
    return client_path().with_name("chat-daemon.exe")


def backend_path():
    return ROOT / ".cache" / "build" / "linux" / "src" / "server" / "chat-server"


def wsl(*args, input=None, timeout=40):
    result = subprocess.run(["wsl.exe", "--cd", str(ROOT), "--", *map(str, args)],
                            input=input, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=timeout,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise RuntimeError(f"WSL command failed ({result.returncode}): {result.stderr[-1500:]}")
    return result.stdout.strip()


def windows_image(pid):
    if not pid:
        return None
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, int(pid))
    if not handle:
        return None
    try:
        size = wintypes.DWORD(32768)
        text = ctypes.create_unicode_buffer(size.value)
        if not kernel.QueryFullProcessImageNameW(handle, 0, text, ctypes.byref(size)):
            return None
        return Path(text.value)
    finally:
        kernel.CloseHandle(handle)


def windows_started(pid):
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    handle = kernel.OpenProcess(0x1000, False, int(pid))
    if not handle:
        return None
    try:
        times = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)):
            raise ctypes.WinError(ctypes.get_last_error())
        return times[0].dwHighDateTime << 32 | times[0].dwLowDateTime
    finally:
        kernel.CloseHandle(handle)


def detached(argv, log, env=None):
    with log.open("wb") as output:
        process = subprocess.Popen([str(part) for part in argv], cwd=ROOT,
                                   stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                                   env=env, close_fds=True,
                                   creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)
    return process


def wait_for(predicate, description, timeout=40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.15)
    raise TimeoutError(description)


def tcp_ready(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=.5):
            return True
    except OSError:
        return False


def provision():
    existing = read_json(STATE / "config.json")
    if existing:
        return existing
    name = f"socket_chat_{PROFILE}"
    password = secrets.token_hex(32)
    marker = secrets.token_hex(32)
    sql = (f"DO $$ BEGIN IF EXISTS (SELECT FROM pg_roles WHERE rolname='{name}') OR "
           f"EXISTS (SELECT FROM pg_database WHERE datname='{name}') THEN "
           "RAISE EXCEPTION 'Hybrid role/database already exists without local configuration; refusing takeover'; "
           "END IF; END $$;\n"
           f"CREATE ROLE {name} LOGIN PASSWORD '{password}';\n"
           f"CREATE DATABASE {name} OWNER {name} ENCODING 'UTF8' TEMPLATE template0;\n"
           f"COMMENT ON DATABASE {name} IS '{marker}';\n"
           f"COMMENT ON ROLE {name} IS '{marker}';\n")
    result = subprocess.run(["wsl.exe", "-u", "postgres", "--", "psql", "-X", "-v", "ON_ERROR_STOP=1"],
                            input=sql, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=40, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise RuntimeError(result.stderr.replace(password, "[redacted]"))
    config = {"tenant": PROFILE, "users": {user: secrets.token_hex(32) for user in USERS},
              "port": 7778 if PROFILE == "demo" else 7777, "database": name, "marker": marker,
              "dsn": f"host=127.0.0.1 port=5432 dbname={name} user={name} password={password}"}
    write_json(STATE / "config.json", config)
    return config


def start_backend(config=None):
    config = config or load_config()
    runtime = read_runtime()
    owned = wsl(*backend_command("_backend-status")) == "running"
    if owned and tcp_ready(config["port"]):
        return
    if tcp_ready(config["port"]):
        raise RuntimeError(f"Port {config['port']} is occupied; refusing to replace another service")
    process = detached(["wsl.exe", "--cd", str(ROOT), "--", *backend_command("_backend")],
                       STATE / "server.log")
    runtime["server"] = {"pid": process.pid, "port": config["port"], "platform": "WSL"}
    write_json(STATE / "runtime.json", runtime)
    def ready():
        error = (STATE / "server.log").read_text(encoding="utf-8", errors="replace")
        if "READY " in error:
            return True
        if process.poll() not in (None, 0):
            raise RuntimeError(f"WSL server exited: {error[-2000:]}")
        return False
    wait_for(ready, "WSL server did not become ready")
    wait_for(lambda: tcp_ready(config["port"]), "Windows cannot reach the WSL loopback server")


def stop_backend():
    wsl(*backend_command("_backend-stop"))
    runtime = read_runtime()
    runtime.pop("server", None)
    write_json(STATE / "runtime.json", runtime)
    wait_for(lambda: not tcp_ready(load_config()["port"]), "Server port did not close")


def start_daemon(user, config):
    runtime = read_runtime()
    previous = runtime.get(user, {})
    image = windows_image(previous.get("pid"))
    if image and image.resolve() == daemon_path().resolve():
        if previous.get("started") and windows_started(previous["pid"]) != previous["started"]:
            raise RuntimeError("Stored daemon PID was reused; refusing to adopt it")
        return
    state = daemon_state(user)
    env = dict(os.environ, CHAT_TOKEN=config["users"][user])
    process = detached([daemon_path(), "--host", "127.0.0.1", "--port", config["port"],
                        "--tenant", config["tenant"], "--user", user, "--state-dir", state],
                       STATE / f"{user}-daemon.log", env)
    runtime[user] = {"pid": process.pid, "platform": "Windows", "state": str(state),
                     "started": windows_started(process.pid)}
    write_json(STATE / "runtime.json", runtime)
    def ready():
        if process.poll() is not None:
            raise RuntimeError(f"{user} daemon exited; inspect its private log")
        return (state / "endpoint").exists() and "READY " in (STATE / f"{user}-daemon.log").read_text(errors="replace")
    wait_for(ready, f"{user} daemon did not become ready")


def start():
    secure_directory(STATE)
    for binary in (client_path(), daemon_path()):
        if not binary.exists():
            raise RuntimeError(f"Missing native Windows build: {binary}")
    wsl("pg_isready", "-q", "-h", "127.0.0.1", "-p", "5432")
    config = provision()
    start_backend(config)
    for user in USERS:
        start_daemon(user, config)
    print(json.dumps(status(), indent=2))


def stop_daemon(user):
    runtime = read_runtime()
    entry = runtime.get(user, {})
    image = windows_image(entry.get("pid"))
    if image:
        if image.resolve() != daemon_path().resolve():
            raise RuntimeError("Stored daemon PID was reused; refusing to signal it")
        if entry.get("started") and windows_started(entry["pid"]) != entry["started"]:
            raise RuntimeError("Stored daemon PID was reused; refusing to signal it")
        # taskkill is called only after validating the exact process image.
        subprocess.run(["taskkill", "/PID", str(entry["pid"]), "/F"], check=True,
                       stdout=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
    runtime.pop(user, None)
    write_json(STATE / "runtime.json", runtime)


def status():
    config = load_config()
    runtime = read_runtime()
    result = {"tenant": config["tenant"], "database": config["database"],
              "server": {"platform": "WSL", "port": config["port"],
                         "windows_reachable": tcp_ready(config["port"]),
                         "linux": read_json(STATE / "server-pid.json", {})}, "daemons": {}}
    for user in USERS:
        entry = runtime.get(user, {})
        image = windows_image(entry.get("pid"))
        result["daemons"][user] = {"pid": entry.get("pid"), "running": bool(image and image.resolve() == daemon_path().resolve()),
                                   "state": str(daemon_state(user))}
    return result


def open_terminals():
    terminal = shutil.which("wt.exe")
    if not terminal:
        raise RuntimeError("Windows Terminal not found; use the attach command from PowerShell")
    arguments = [terminal, "-w", "new", "new-tab", "--title", "Socket Chats - Alice",
                 "--startingDirectory", str(ROOT), str(client_path()), "--state-dir", str(daemon_state("alice")),
                 ";", "split-pane", "--horizontal", "--title", "Socket Chats - Bob",
                 "--startingDirectory", str(ROOT), str(client_path()), "--state-dir", str(daemon_state("bob"))]
    subprocess.run(arguments, check=True)


def backend():
    config = load_config()
    binary = backend_path()
    started = Path(f"/proc/{os.getpid()}/stat").read_text().rsplit(")", 1)[1].split()[19]
    write_json(STATE / "server-pid.json", {"pid": os.getpid(), "executable": str(binary),
                                         "platform": "Linux", "started": started})
    # wsl.exe may attach a Linux PTY even when its Windows parent is detached.
    # Redirect inside Linux so service logs do not depend on that console bridge.
    with (STATE / "server.log").open("ab", buffering=0) as output:
        os.dup2(output.fileno(), 1)
        os.dup2(output.fileno(), 2)
    environment = dict(os.environ, CHAT_DB_DSN=config["dsn"],
                       CHAT_CREDENTIALS=";".join(f"{config['tenant']}:{user}:{token}" for user, token in config["users"].items()))
    os.execve(binary, [str(binary), "--host", "127.0.0.1", "--port", str(config["port"]), "--threads", "4"], environment)


def backend_stop():
    entry = read_json(STATE / "server-pid.json", {})
    pid = entry.get("pid")
    if not pid or not Path(f"/proc/{pid}").exists():
        return
    if Path(f"/proc/{pid}/exe").resolve() != backend_path().resolve():
        raise RuntimeError("Stored Linux PID was reused; refusing to signal it")
    started = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]
    if entry.get("started") and entry["started"] != started:
        raise RuntimeError("Stored Linux PID was reused; refusing to signal it")
    os.kill(pid, signal.SIGTERM)
    wait_for(lambda: not Path(f"/proc/{pid}").exists(), "WSL server did not exit", timeout=25)
    (STATE / "server-pid.json").unlink(missing_ok=True)


def backend_status():
    entry = read_json(STATE / "server-pid.json", {})
    pid = entry.get("pid")
    running = bool(pid and Path(f"/proc/{pid}/exe").resolve() == backend_path().resolve())
    if running and entry.get("started"):
        running = entry["started"] == Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]
    print("running" if running else "stopped")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["start", "status", "stop", "restart-server", "attach", "open", "verify", "_backend", "_backend-stop", "_backend-status"])
    parser.add_argument("--user", choices=USERS, default="alice")
    parser.add_argument("--profile", choices=("hybrid", "demo"), default="hybrid")
    args = parser.parse_args()
    select_profile(args.profile)
    if args.command == "_backend":
        backend()
        return
    if args.command == "_backend-stop":
        backend_stop()
        return
    if args.command == "_backend-status":
        backend_status()
        return
    if os.name != "nt":
        parser.error("Control the hybrid stack from Windows Python")
    if args.command == "start":
        start()
    elif args.command == "status":
        print(json.dumps(status(), indent=2))
    elif args.command == "stop":
        for user in USERS:
            stop_daemon(user)
        stop_backend()
        print("Application stopped. PostgreSQL and persistent data were left untouched.")
    elif args.command == "restart-server":
        stop_backend()
        start_backend()
    elif args.command == "attach":
        subprocess.run([str(client_path()), "--state-dir", str(daemon_state(args.user))], check=True)
    elif args.command == "open":
        open_terminals()
    elif args.command == "verify":
        subprocess.run([sys.executable, str(ROOT / "tests" / "hybrid_journey.py")], check=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"hybrid: {error}", file=sys.stderr)
        sys.exit(1)
