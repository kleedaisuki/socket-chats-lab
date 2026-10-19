"""Additional bounded-window PTY scenarios; invoked by tui_state_journey."""
from __future__ import annotations

import threading

from journey import require
from terminal_journey import Terminal
from tui_ipc_fixture import ScriptedIPC
from wire import pack


def message(number, request=0):
    text = "LIVE_800" if number == 800 else f"WINDOW_{number:03d}"
    return pack(4, request, [str(number), "lobby", "bob", "1700000000000", text, f"window-{number}"])


class WindowIPC(ScriptedIPC):
    def __init__(self, directory):
        self.older_loaded = threading.Event()
        self.recent_loaded = threading.Event()
        self.recent_requests = 0
        super().__init__(directory)

    def greeting(self, hello):
        return (pack(2, hello.request, ["alpha", "alice"]) +
                pack(16, 0, ["cache", "cached messages"]) +
                b"".join(message(number) for number in range(301, 801)) +
                pack(16, 0, ["online", "connected"]))

    def respond(self, connection, frame):
        if frame.kind == 12:
            connection.sendall(pack(13, frame.request, ["lobby", "800", "0"]))
        elif frame.kind == 5:
            self.history_requests.append(frame.fields)
            after = int(frame.fields[1])
            if after == 600:
                self.recent_requests += 1
                frames = b"".join(message(number, frame.request) for number in range(601, 801))
                connection.sendall(frames + pack(6, frame.request, ["lobby", "800"]))
                if self.recent_requests > 1:
                    self.recent_loaded.set()
            elif after == 101:
                frames = b"".join(message(number, frame.request) for number in range(102, 301))
                connection.sendall(frames + pack(6, frame.request, ["lobby", "300"]))
                self.older_loaded.set()
            else:
                # A boundary PgUp may ask for yet older history; there is none in this fixture.
                connection.sendall(pack(6, frame.request, ["lobby", str(after)]))
        elif frame.kind == 3:
            self.send_fields = frame.fields
            self.sent.set()
            connection.sendall(pack(11, frame.request,
                                    ["delivery_unknown", "RAW_BACKEND_DIAGNOSTIC_DO_NOT_DISPLAY"]))
        elif frame.kind == 9:
            connection.sendall(pack(10, frame.request, frame.fields))
        return True


class PartialWindowIPC(WindowIPC):
    def __init__(self, directory):
        self.partial_ready = threading.Event()
        self.release_end = threading.Event()
        self.stale_continuation = threading.Event()
        super().__init__(directory)

    def respond(self, connection, frame):
        if frame.kind == 12 and self.recent_requests > 1:
            connection.sendall(pack(13, frame.request, ["lobby", "801", "0"]))
            return True
        if frame.kind != 5:
            return super().respond(connection, frame)
        after = int(frame.fields[1])
        if after == 101:
            self.history_requests.append(frame.fields)
            connection.sendall(b"".join(message(number, frame.request) for number in range(102, 151)))
            self.partial_ready.set()
            require(self.release_end.wait(12), "held short history page was not released")
            connection.sendall(pack(6, frame.request, ["lobby", "150"]))
            return True
        if after == 150:
            self.history_requests.append(frame.fields)
            self.stale_continuation.set()
            connection.sendall(b"".join(message(number, frame.request) for number in range(151, 301)) +
                               pack(6, frame.request, ["lobby", "300"]))
            return True
        returning = after == 600 and self.recent_requests >= 1
        handled = super().respond(connection, frame)
        if returning:
            connection.sendall(pack(4, 0, ["801", "lobby", "bob", "1700000000000", "LIVE_801", "n801"]))
        return handled

    def close(self):
        self.release_end.set()
        super().close()


def page_until(ui, key, needle, limit=160):
    for count in range(limit):
        if needle in ui.screen.text():
            return count
        ui.write(key)
        ui.drain(.025)
    ui.wait(needle, timeout=3)
    return limit


def partial_page_return(args, work, env):
    fixture = PartialWindowIPC(work / "partial-window")
    ui = None
    try:
        ui = Terminal([args.client, "--state-dir", str(work / "partial-window")], env)
        ui.wait("LIVE_800")
        ui.drain(.3)
        require(fixture.recent_requests == 1, "partial-page fixture initial sync")
        ui.command("/history")
        require(fixture.partial_ready.wait(4), "short older page was not requested")
        ui.wait("WINDOW_751")
        # Stay at the bottom while history-end is withheld: PgDn changes intent,
        # not a scroll offset, and must cancel the pending older-page chain.
        ui.write(b"\x1b[6~")
        ui.wait("正在返回最新消息")
        fixture.release_end.set()
        require(fixture.recent_loaded.wait(4), "latest intent must refetch after held history-end")
        require(not fixture.stale_continuation.is_set(),
                "returning live during partial history must cancel stale continuation after 150")
        ui.wait("LIVE_800")
        ui.wait("LIVE_801")
        require("WINDOW_102" not in ui.screen.text(), "latest window must drop oldest retained history")
        require(fixture.recent_requests == 2, "return-to-live must perform one fresh recent sync")
        ui.write(b"\x03")
        ui.finish()
        print("PASS partial window: held end=150, PgDn cancels older continuation, recent_fetches=2, live=800/801 retained")
    finally:
        if ui:
            ui.close()
        fixture.close()


def window_and_delivery(args, work, env):
    fixture = WindowIPC(work / "window")
    ui = None
    try:
        ui = Terminal([args.client, "--state-dir", str(work / "window")], env)
        ui.wait("LIVE_800")
        ui.drain(.3)
        require(fixture.recent_requests == 1, "initial cached-window sync must fetch recent range once")
        require(ui.screen.text().count("LIVE_800") == 1, "500 cache plus history duplicates must deduplicate")
        ui.command("/history")
        require(fixture.older_loaded.wait(4), "explicit history did not request older 102..300 window")
        ui.wait("已加载较早历史")
        ui.wait("WINDOW_601")
        require("LIVE_800" not in ui.screen.text(),
                "500-message older window must evict the newest settled tail, not grow unbounded")
        up_pages = page_until(ui, b"\x1b[5~", "WINDOW_102")
        require("LIVE_800" not in ui.screen.text(), "older window must not retain only the newest tail")
        down_pages = page_until(ui, b"\x1b[6~", "LIVE_800")
        require(fixture.recent_loaded.wait(3), "return-to-bottom must refetch recent window after newest eviction")
        require(fixture.recent_requests >= 2, "recent window must be explicitly resynchronized")
        ui.command("UNCERTAIN_ERROR_MESSAGE")
        ui.wait("待确认")
        require(fixture.sent.wait(2) and fixture.send_fields[2] == "UNCERTAIN_ERROR_MESSAGE",
                "scripted delivery_unknown must correspond to the actual send")
        require("发送失败" not in ui.screen.text(), "uncertain upstream result must not be presented as safe failure")
        require("RAW_BACKEND_DIAGNOSTIC" not in ui.screen.text() and "delivery_unknown" not in ui.screen.text(),
                "UI must present actionable human guidance instead of raw error code/diagnostic")
        ui.wait("避免重复发送")
        ui.write("错误后仍可写草稿")
        ui.wait("错误后仍可写草稿")
        ui.write(b"\x03")
        ui.finish()
        print(f"PASS window: cached=500, older=199, PgUp={up_pages}, PgDn={down_pages}, "
              f"recent_fetches={fixture.recent_requests}, explicit delivery_unknown=uncertain")
    finally:
        if ui:
            ui.close()
        fixture.close()
    partial_page_return(args, work, env)
