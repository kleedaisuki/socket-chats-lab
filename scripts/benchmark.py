"""Reproducible native-process chat latency/fanout smoke benchmark.

The server is launched locally, every client drains its broadcast stream, and
latency measures durable send->ack. This is not a maximum-capacity claim.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import platform
import statistics
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from journey import Process  # noqa: E402
from wire import Peer  # noqa: E402


def percentile(samples, q):
    return sorted(samples)[min(len(samples) - 1, max(0, int(q * len(samples)) - 1))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--clients", type=int, default=16)
    parser.add_argument("--messages", type=int, default=100)
    parser.add_argument("--bytes", type=int, default=256)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--postgres", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT / ".cache" / "benchmark.json")
    args = parser.parse_args()
    if not 1 <= args.clients <= 128 or not 1 <= args.messages <= 10000 or not 16 <= args.bytes <= 65536:
        parser.error("clients 1..128, messages 1..10000, bytes 16..65536")
    output = args.output.resolve()
    if not output.is_relative_to(ROOT):
        parser.error("output must stay inside repository")
    temp = ROOT / ".temp"
    temp.mkdir(exist_ok=True)
    env = dict(os.environ, CHAT_CREDENTIALS="bench:writer:local-benchmark")
    argv = [str(args.server.resolve()), "--port", "0", "--threads", str(args.threads)]
    if not args.postgres:
        argv.append("--memory")
    with tempfile.TemporaryDirectory(prefix="benchmark-", dir=temp) as directory:
        server = Process(argv, env, Path(directory), "server")
        try:
            port = server.ready()
            with Peer(port) as ping:
                ping.hello("bench", "writer", "local-benchmark")
                ping_times = []
                for _ in range(100):
                    started = time.perf_counter()
                    request = ping.send(9, [])
                    ping.wait(10, request)
                    ping_times.append((time.perf_counter() - started) * 1000)
            prefix = "中文🌸 "
            text = prefix + "x" * (args.bytes - len(prefix.encode("utf-8")))
            barrier = threading.Barrier(args.clients)
            run_id = str(time.time_ns())

            def session(index):
                with Peer(port) as peer:
                    peer.hello("bench", "writer", "local-benchmark")
                    barrier.wait(timeout=20)
                    latencies = []
                    for number in range(args.messages):
                        started = time.perf_counter()
                        request = peer.send(3, ["lobby", f"b-{run_id}-{index}-{number}", text])
                        while True:
                            frame = peer.receive(timeout=30)
                            if frame.kind == 11:
                                raise RuntimeError(frame.fields)
                            if frame.kind == 15 and frame.request == request:
                                break
                        latencies.append((time.perf_counter() - started) * 1000)
                    return latencies

            started = time.perf_counter()
            with ThreadPoolExecutor(max_workers=args.clients) as workers:
                samples = [sample for part in workers.map(session, range(args.clients)) for sample in part]
            elapsed = time.perf_counter() - started
            result = {
                "platform": platform.platform(), "python": platform.python_version(),
                "server": str(args.server.resolve()), "backend": "postgres" if args.postgres else "memory",
                "clients": args.clients, "fanout": "all same-tenant room clients",
                "server_threads": args.threads, "message_bytes": len(text.encode("utf-8")),
                "messages": len(samples), "elapsed_seconds": elapsed,
                "acknowledgements_per_second": len(samples) / elapsed,
                "ping_ms": {"p50": statistics.median(ping_times), "p95": percentile(ping_times, .95)},
                "send_ack_ms": {"p50": statistics.median(samples), "p95": percentile(samples, .95),
                                "p99": percentile(samples, .99), "max": max(samples)},
                "notes": "Local smoke benchmark, one outstanding send/client; includes auth/thread startup in throughput elapsed. No capacity claim.",
            }
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(result, indent=2))
        finally:
            server.close()


if __name__ == "__main__":
    main()
