"""POSIX FTXUI recent-history and uncertain-send journeys, including scripted IPC."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import tempfile

from journey import CREDENTIALS, Process, ROOT, require
from terminal_journey import Terminal, publish
from wire import Peer
from tui_ipc_fixture import ScriptedIPC


def recent_history(args, work, env):
    server = Process([args.server, "--memory", "--port", "0"], env, work, "recent-server")
    daemon = None
    ui = None
    try:
        port = server.ready()
        with Peer(port) as bob:
            bob.hello("alpha", "bob", "bob-secret")
            bob.wait(15, bob.send(14, ["other"]))
            for index in range(300):
                publish(bob, f"PRESEED_{index:03d}")
                # Interleaved global IDs must not be confused with per-room positions.
                if index % 10 == 0:
                    request = bob.send(3, ["other", f"gap-{index}", f"OTHER_{index:03d}"])
                    bob.wait(15, request)
                    bob.wait(4)
        state = work / "fresh"
        require(not state.exists(), "fresh daemon must have no cached events")
        daemon = Process([args.daemon, "--port", str(port), "--tenant", "alpha", "--user", "alice",
                          "--state-dir", str(state)], env, work, "recent-daemon")
        daemon.ready()
        require(not (state / "events.jsonl").exists() or not (state / "events.jsonl").read_bytes(),
                "initial recent-history must be tested without daemon cache")
        ui = Terminal([args.client, "--state-dir", str(state)], env)
        ui.wait("PRESEED_299", timeout=20)
        ui.drain(.3)
        require("PRESEED_000" not in ui.screen.text(), "fresh attach displayed the first page instead of recent tail")
        require("OTHER_" not in ui.screen.text(), "other room history leaked into active room")
        ui.write(b"\x03")
        ui.finish()
    finally:
        if ui:
            ui.close()
        if daemon:
            daemon.close()
        server.close()


def uncertain_and_cursor(args, work, env):
    fixture = ScriptedIPC(work / "scripted")
    ui = None
    try:
        ui = Terminal([args.client, "--state-dir", str(work / "scripted")], env)
        ui.wait("HISTORY_GAP_300")
        ui.wait("LOBBY_LIVE_400")
        require("OTHER_ROOM_LIVE" not in ui.screen.text(), "scripted inactive live room leaked")
        require(len(fixture.history_requests) >= 2 and fixture.history_requests[1][1] == "200",
                "synchronization must follow fetched cursor, not room advertised/live max")
        ui.command("UNCERTAIN_SENT")
        ui.wait("发送中")
        require(fixture.sent.wait(3), "scripted daemon never received optimistic send")
        require(fixture.send_fields[0] == "lobby" and fixture.send_fields[2] == "UNCERTAIN_SENT",
                "scripted daemon received wrong send fields")
        ui.write("未发送草稿 🌷")
        ui.wait("未发送草稿 🌷")
        fixture.stop.set()
        ui.wait("待确认")
        ui.wait("未发送草稿 🌷")
        require("发送中" not in ui.screen.text(), "disconnected in-flight send remained pending forever")
        ui.write("，继续编辑")
        ui.wait("未发送草稿 🌷，继续编辑")
        require(ui.process.poll() is None, "daemon loss must not kill editable UI")
        ui.write(b"\x03")
        ui.finish()
    finally:
        if ui:
            ui.close()
        fixture.close()


def main():
    parser = argparse.ArgumentParser()
    for name in ("server", "daemon", "client"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    if os.name != "posix":
        parser.error("requires POSIX PTY")
    for name in ("server", "daemon", "client"):
        setattr(args, name, str(Path(getattr(args, name)).resolve()))
    (ROOT / ".temp").mkdir(exist_ok=True)
    env = dict(os.environ, CHAT_CREDENTIALS=CREDENTIALS, CHAT_TOKEN="alice-secret",
               TERM="xterm-256color", COLORTERM="truecolor", LANG="C.UTF-8", TZ="Asia/Shanghai")
    with tempfile.TemporaryDirectory(prefix="tui-state-", dir=ROOT / ".temp") as directory:
        work = Path(directory)
        recent_history(args, work, env)
        uncertain_and_cursor(args, work, env)
        from tui_window_journey import window_and_delivery
        window_and_delivery(args, work, env)
    print("PASS TUI state: fresh recent history, cursor isolation, ACK-loss/explicit uncertainty, retained editable draft, 500-message older-window retention and return to live, terminal restoration")


if __name__ == "__main__":
    main()
