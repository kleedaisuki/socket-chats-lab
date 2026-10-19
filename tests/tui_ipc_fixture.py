"""Shared authenticated scripted IPC fixture for real FTXUI PTY tests."""
import socket
import threading

from journey import require
from wire import Peer, pack


class ScriptedIPC:
    def __init__(self, directory):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(10)
        directory.mkdir()
        (directory / "endpoint").write_text(
            f"{self.listener.getsockname()[1]}\nipc-test-secret\nalpha\nalice\n", encoding="utf-8")
        self.stop = threading.Event()
        self.sent = threading.Event()
        self.errors = []
        self.send_fields = None
        self.history_requests = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def greeting(self, hello):
        return pack(2, hello.request, ["alpha", "alice"]) + pack(16, 0, ["online", "connected"])

    def respond(self, connection, frame):
        return False

    def run(self):
        connection = None
        try:
            connection, _ = self.listener.accept()
            peer = Peer.__new__(Peer)
            peer.socket, peer.buffer, peer.pending, peer.next_id = connection, bytearray(), [], 1
            hello = peer.receive()
            require(hello.kind == 1 and hello.fields == ["alpha", "alice", "ipc-test-secret"],
                    "scripted daemon IPC authentication")
            connection.sendall(self.greeting(hello))
            while not self.stop.is_set():
                try:
                    frame = peer.receive(.1)
                except (TimeoutError, socket.timeout):
                    continue
                if self.respond(connection, frame):
                    continue
                if frame.kind == 12:
                    connection.sendall(pack(13, frame.request, ["lobby", "350", "0", "other", "900", "0"]))
                elif frame.kind == 5:
                    self.history_requests.append(frame.fields)
                    after = int(frame.fields[1])
                    if len(self.history_requests) == 1:
                        require(frame.fields == ["lobby", "150", "200"],
                                f"initial fetch must be recent advertised window: {frame.fields}")
                        frames = [pack(4, frame.request, ["200", "lobby", "bob", "1700000000000", "HISTORY_FIRST", "n200"]),
                                  pack(4, 0, ["900", "other", "bob", "1700000000000", "OTHER_ROOM_LIVE", "n900"]),
                                  pack(4, 0, ["400", "lobby", "bob", "1700000000000", "LOBBY_LIVE_400", "n400"]),
                                  pack(6, frame.request, ["lobby", "200"])]
                    elif len(self.history_requests) == 2:
                        require(after == 200, f"live/metadata maxima must not skip fetched history cursor: {after}")
                        frames = [pack(4, frame.request, ["300", "lobby", "bob", "1700000000000", "HISTORY_GAP_300", "n300"]),
                                  pack(4, frame.request, ["350", "lobby", "bob", "1700000000000", "HISTORY_TARGET_350", "n350"]),
                                  pack(6, frame.request, ["lobby", "350"])]
                    else:
                        frames = [pack(6, frame.request, ["lobby", str(after)])]
                    connection.sendall(b"".join(frames))
                elif frame.kind == 3:
                    self.send_fields = frame.fields
                    self.sent.set()
                    # Deliberately withhold ACK: the UI must classify loss as uncertain.
                elif frame.kind == 9:
                    connection.sendall(pack(10, frame.request, frame.fields))
        except (EOFError, ConnectionResetError, BrokenPipeError):
            pass
        except BaseException as error:
            self.errors.append(error)
        finally:
            if connection:
                connection.close()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=12)
        self.listener.close()
        require(not self.thread.is_alive(), "scripted IPC fixture did not stop")
        if self.errors:
            raise self.errors[0]
