"""Real process reconnect, cache replay, privacy, and terminal safety checks."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from journey import CREDENTIALS, Process, ROOT, endpoint, require, send_message
from wire import Peer


def windows_private_state(state, names):
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes

    class AclHeader(ctypes.Structure):
        _fields_ = [("revision", wintypes.BYTE), ("reserved", wintypes.BYTE),
                    ("size", wintypes.WORD), ("count", wintypes.WORD),
                    ("reserved2", wintypes.WORD)]

    class AllowedAce(ctypes.Structure):
        _fields_ = [("type", wintypes.BYTE), ("flags", wintypes.BYTE),
                    ("size", wintypes.WORD), ("mask", wintypes.DWORD),
                    ("sid_start", wintypes.DWORD)]

    security = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    security.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                         ctypes.POINTER(wintypes.HANDLE)]
    security.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                            wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    security.GetNamedSecurityInfoW.argtypes = [wintypes.LPWSTR, ctypes.c_int, wintypes.DWORD,
                                              ctypes.c_void_p, ctypes.c_void_p,
                                              ctypes.c_void_p, ctypes.c_void_p,
                                              ctypes.POINTER(ctypes.c_void_p)]
    security.GetAce.argtypes = [ctypes.c_void_p, wintypes.DWORD,
                                ctypes.POINTER(ctypes.c_void_p)]
    security.EqualSid.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    security.ConvertSecurityDescriptorToStringSecurityDescriptorW.argtypes = [
        ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
        ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    token = wintypes.HANDLE()
    require(security.OpenProcessToken(kernel.GetCurrentProcess(), 8, ctypes.byref(token)),
            "cannot inspect current token SID")
    size = wintypes.DWORD()
    security.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
    info = ctypes.create_string_buffer(size.value)
    try:
        require(security.GetTokenInformation(token, 1, info, size, ctypes.byref(size)),
                "cannot inspect token user")
    finally:
        kernel.CloseHandle(token)
    sid = ctypes.cast(info, ctypes.POINTER(ctypes.c_void_p))[0]
    for name in names:
        path = state/name if name else state
        descriptor = ctypes.c_void_p()
        dacl = ctypes.c_void_p()
        code = security.GetNamedSecurityInfoW(str(path), 1, 4, None, None, ctypes.byref(dacl), None,
                                             ctypes.byref(descriptor))
        require(code == 0, f"cannot inspect {path} security: {code}")
        text = ctypes.c_void_p()
        try:
            require(security.ConvertSecurityDescriptorToStringSecurityDescriptorW(
                descriptor, 1, 4, ctypes.byref(text), None), "cannot format state DACL")
            sddl = ctypes.wstring_at(text)
            require(bool(dacl), f"{path} must not have a null DACL")
            count = ctypes.cast(dacl, ctypes.POINTER(AclHeader)).contents.count
            require(count > 0, f"{path} must grant access to the current user")
            for index in range(count):
                ace = ctypes.c_void_p()
                require(security.GetAce(dacl, index, ctypes.byref(ace)), "cannot inspect state ACE")
                allowed = ctypes.cast(ace, ctypes.POINTER(AllowedAce)).contents
                require(allowed.type == 0 and
                        security.EqualSid(sid, ace.value + AllowedAce.sid_start.offset),
                        f"{path} must allow only current user, not Everyone/Users: {sddl}")
            if not name:
                require(sddl.startswith("D:P"), "state directory must block ACL inheritance")
        finally:
            if text.value:
                kernel.LocalFree(text)
            kernel.LocalFree(descriptor)


def status(peer, state, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = peer.wait(16, timeout=deadline-time.monotonic())
        if frame.fields[0] == state:
            return
    raise AssertionError(f"No daemon status {state}")


def main():
    parser = argparse.ArgumentParser()
    for key in ("server", "daemon", "client"):
        parser.add_argument("--"+key, required=True)
    args = parser.parse_args()
    for key in ("server", "daemon", "client"):
        setattr(args, key, str(Path(getattr(args, key)).resolve()))
    temp = ROOT / ".temp"
    temp.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="client-", dir=temp) as directory:
        work = Path(directory)
        state = work / "state"
        env = dict(os.environ, CHAT_CREDENTIALS=CREDENTIALS, CHAT_TOKEN="alice-secret")
        if os.name == "nt":
            state.mkdir()
            (state/"identity").write_bytes(b"alpha\nalice\n")
            (state/"endpoint").write_bytes(b"1\nstale-token\nalpha\nalice\n")
            (state/"events.jsonl").touch()
            (state/"lock").touch()
            # Exercise migration of pre-existing broad inherited and explicit ACLs.
            acl = subprocess.run(["icacls", str(state), "/grant", "*S-1-1-0:(OI)(CI)F", "/T"],
                                 capture_output=True, text=True, timeout=10)
            require(acl.returncode == 0, "cannot prepare broad Windows ACL fixture")
        server = Process([args.server, "--memory", "--port", "0"], env, work, "server1")
        daemon = None
        ui = None
        try:
            port = server.ready()
            daemon_argv = [args.daemon, "--port", str(port), "--tenant", "alpha",
                           "--user", "alice", "--state-dir", str(state)]
            daemon = Process(daemon_argv, env, work, "daemon")
            ipc = daemon.ready()
            _, secret = endpoint(state)
            ui = Peer(ipc)
            ui.hello("alpha", "alice", secret)
            status(ui, "online")
            duplicate = subprocess.run(daemon_argv, env=env, capture_output=True,
                                       text=True, timeout=10)
            require(duplicate.returncode != 0 and "already in use" in duplicate.stderr,
                    "second daemon must not acquire writer lock")
            text = "你好 🌷\x1b[31m untrusted title \x1b]0;injected\x07"
            with Peer(port) as bob:
                bob.hello("alpha", "bob", "bob-secret")
                send_message(bob, text)
                ui.wait(4, 0)
                require(bob.rooms()["lobby"][1] == 0, "own messages are not unread")
            require(ui.rooms()["lobby"][1] == 1, "daemon reception must not auto-mark read")
            windows_private_state(state, ["", "identity", "endpoint", "events.jsonl"])
            server.close()
            status(ui, "offline")
            request = ui.send(12, [])
            require(ui.wait(11, request).fields[0] == "offline", "offline sends must fail visibly")
            result = subprocess.run([args.client, "--state-dir", str(state), "--plain"],
                                    input="/rooms\n/quit\n", env=env, capture_output=True,
                                    text=True, encoding="utf-8", timeout=10)
            require(result.returncode == 0, result.stderr)
            require("cached" in result.stdout and "你好 🌷" in result.stdout,
                    "offline UI must clearly label cached UTF-8 replay")
            require("\x1b" not in result.stdout and "\x07" not in result.stdout,
                    "terminal escape injection")
            server = Process([args.server, "--memory", "--port", str(port)],
                             env, work, "server2")
            server.ready()
            status(ui, "online", timeout=20)
            require("lobby" in ui.rooms(), "existing IPC client must recover after upstream reconnect")
            with Peer(port) as bob:
                bob.hello("alpha", "bob", "bob-secret")
                send_message(bob, "after reconnect")
                require(ui.wait(4, 0).fields[4] == "after reconnect", "live events after reconnect")
            ui.close()
            ui = None
            daemon.close()
            daemon = None
            windows_private_state(state, ["", "identity", "endpoint", "events.jsonl", "lock"])
            other = subprocess.run([args.daemon, "--tenant", "beta", "--user", "alice",
                                    "--state-dir", str(state)], env=env, capture_output=True,
                                   text=True, timeout=10)
            require(other.returncode != 0 and "different account" in other.stderr,
                    "cached messages must not leak between accounts")
            lines = (state/"events.jsonl").read_text(encoding="utf-8").splitlines()
            require(all(json.loads(line)["type"] in (4, 8) for line in lines), "cache event types")
            require("alice-secret" not in "\n".join(lines) and secret not in "\n".join(lines),
                    "authentication secrets must not be journaled")
            print("PASS client: reconnect, persistent offline replay, explicit read, writer lock, identity, ANSI safety")
        finally:
            if ui:
                ui.close()
            if daemon:
                daemon.close()
            server.close()


if __name__ == "__main__":
    main()
