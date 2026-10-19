"""Independent SCL/1 peer for integration tests and operational probes."""

from __future__ import annotations

from dataclasses import dataclass
import socket
import struct
import time

HEADER = struct.Struct("!4sBBHIII")
LIMIT = 1 << 20


@dataclass
class Frame:
    kind: int
    request: int
    fields: list[str]
    compressed: bool = False


def pack(kind: int, request: int, fields: list[str]) -> bytes:
    encoded = [field.encode("utf-8") for field in fields]
    body = struct.pack("!H", len(encoded))
    body += b"".join(struct.pack("!I", len(field)) + field for field in encoded)
    return HEADER.pack(b"SCL1", 1, kind, 0, request, len(body), len(body)) + body


def unpack_body(body: bytes, raw_size: int, flags: int) -> list[str]:
    if flags & ~1:
        raise ValueError("reserved frame flags")
    if flags:
        out = bytearray()
        offset = 0
        while offset < len(body):
            token = body[offset]
            offset += 1
            if token < 128:
                count = token + 1
                if offset + count > len(body):
                    raise ValueError("short literal")
                out.extend(body[offset:offset + count])
                offset += count
            else:
                if offset + 2 > len(body):
                    raise ValueError("short match")
                distance = struct.unpack_from("!H", body, offset)[0]
                offset += 2
                if not 0 < distance <= len(out):
                    raise ValueError("invalid match distance")
                for _ in range((token & 127) + 3):
                    out.append(out[-distance])
            if len(out) > raw_size:
                raise ValueError("compression overflow")
        body = bytes(out)
    if len(body) != raw_size or len(body) < 2:
        raise ValueError("incorrect payload size")
    count = struct.unpack_from("!H", body)[0]
    if count > 128:
        raise ValueError("too many fields")
    fields = []
    offset = 2
    for _ in range(count):
        size = struct.unpack_from("!I", body, offset)[0]
        offset += 4
        if offset + size > len(body):
            raise ValueError("truncated field")
        fields.append(body[offset:offset + size].decode("utf-8", errors="strict"))
        offset += size
    if offset != len(body):
        raise ValueError("trailing payload")
    return fields


class Peer:
    def __init__(self, port: int, host: str = "127.0.0.1"):
        self.socket = socket.create_connection((host, port), timeout=5)
        self.buffer = bytearray()
        self.pending: list[Frame] = []
        self.next_id = 1

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def send(self, kind: int, fields: list[str], *, fragments: bool = False) -> int:
        request = self.next_id
        self.next_id += 1
        data = pack(kind, request, fields)
        if fragments:
            for index in range(0, len(data), 3):
                self.socket.sendall(data[index:index + 3])
        else:
            self.socket.sendall(data)
        return request

    def receive(self, timeout: float = 5) -> Frame:
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= HEADER.size:
                magic, version, kind, flags, request, size, raw = HEADER.unpack_from(self.buffer)
                if magic != b"SCL1" or version != 1 or max(size, raw) > LIMIT:
                    raise ValueError("invalid SCL header")
                total = HEADER.size + size
                if len(self.buffer) >= total:
                    fields = unpack_body(bytes(self.buffer[HEADER.size:total]), raw, flags)
                    del self.buffer[:total]
                    return Frame(kind, request, fields, bool(flags))
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("waiting for SCL frame")
            self.socket.settimeout(remaining)
            chunk = self.socket.recv(65536)
            if not chunk:
                raise EOFError("peer disconnected")
            self.buffer.extend(chunk)

    def wait(self, kind: int, request: int | None = None, timeout: float = 8) -> Frame:
        def matches(frame):
            return frame.kind == kind and (request is None or frame.request == request)

        for index, frame in enumerate(self.pending):
            if matches(frame):
                return self.pending.pop(index)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self.receive(deadline - time.monotonic())
            if frame.kind == 11 and kind != 11 and (request is None or frame.request == request):
                raise AssertionError(f"server error: {frame.fields}")
            if matches(frame):
                return frame
            self.pending.append(frame)
        raise TimeoutError(f"waiting for type {kind}, request {request}")

    def hello(self, tenant: str, user: str, token: str, *, fragments: bool = False):
        request = self.send(1, [tenant, user, token], fragments=fragments)
        frame = self.wait(2, request)
        assert frame.fields[:2] == [tenant, user], frame

    def history(self, room: str = "lobby", after: str = "0", limit: int = 200) -> list[Frame]:
        request = self.send(5, [room, after, str(limit)])
        self.wait(6, request)
        messages = [f for f in self.pending if f.kind == 4 and f.request == request]
        self.pending = [f for f in self.pending if f not in messages]
        return messages

    def rooms(self) -> dict[str, tuple[int, int]]:
        request = self.send(12, [])
        fields = self.wait(13, request).fields
        assert len(fields) % 3 == 0, fields
        return {fields[i]: (int(fields[i + 1]), int(fields[i + 2]))
                for i in range(0, len(fields), 3)}
