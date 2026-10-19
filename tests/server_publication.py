"""Real-socket regressions for durable publication and per-room event ordering."""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import socket
import subprocess
from wire import HEADER, Peer, pack


def exercise(port: int):
    with Peer(port) as attacker:
        attacker.socket.sendall(HEADER.pack(b"SCL1",1,1,0,1,1<<20,1<<20)+b"x"*8193)
        try:
            assert attacker.socket.recv(1)==b"", "preauth buffer limit must disconnect"
        except ConnectionResetError:
            pass
    with Peer(port) as observer:
        observer.hello("t", "observer", "observer-secret")
        # FIN after a complete send closes the requester before worker publication.
        for i in range(20):
            with Peer(port) as sender:
                sender.hello("t", "sender", "sender-secret")
                sender.send(3, ["lobby", f"disconnect-{i}", "persist even after requester FIN"])
                sender.socket.shutdown(socket.SHUT_WR)
                event = observer.wait(4, 0)
                assert event.fields[5] == f"disconnect-{i}", event

        # One coalesced batch followed by FIN must not discard queued commands.
        with Peer(port) as sender:
            sender.hello("t", "sender", "sender-secret")
            requests=list(range(100,132))
            sender.socket.sendall(b"".join(pack(3,request,["lobby",f"batch-{request}","batched FIN"])
                                            for request in requests))
            sender.socket.shutdown(socket.SHUT_WR)
            for request in requests:
                assert observer.wait(4,0).fields[5]==f"batch-{request}"
                assert sender.wait(15,request).fields[1]==f"batch-{request}"
            # All replies/events precede the server's graceful output close.
            while True:
                try:sender.receive()
                except EOFError:break

        def write(index: int):
            with Peer(port) as sender:
                sender.hello("t", "sender", "sender-secret")
                for i in range(30):
                    request = sender.send(3, ["lobby", f"parallel-{index}-{i}", "ordered publication"])
                    sender.wait(15, request)

        with ThreadPoolExecutor(max_workers=8) as pool:
            tasks = [pool.submit(write, i) for i in range(8)]
            ids = [int(observer.wait(4, 0).fields[0]) for _ in range(240)]
            for task in tasks:
                task.result()
        assert len(set(ids)) == 240 and ids == sorted(ids), ids
        print("PASS requester FIN durable broadcasts and 240 concurrent same-room publications")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    args = parser.parse_args()
    env = dict(os.environ, CHAT_CREDENTIALS="t:observer:observer-secret;t:sender:sender-secret")
    process = subprocess.Popen([args.server, "--memory", "--port", "0", "--threads", "8"],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, encoding="utf-8")
    try:
        ready = process.stdout.readline().strip()
        assert ready.startswith("READY "), ready
        exercise(int(ready.rsplit(":", 1)[1]))
    finally:
        process.terminate()
        try:
            process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
