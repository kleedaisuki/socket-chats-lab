"""Terminate only this dedicated test database's client connections and recover.

Run against an isolated database: this test intentionally interrupts all sessions
using CHAT_DB_DSN's database/user, except its own psql administrative statement.
It does not stop or restart the PostgreSQL cluster.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from journey import Process, ROOT, require, send_message
from wire import Peer

sys.path.insert(0, str(ROOT / "scripts"))
from pg_tools import psql_connection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True)
    args = parser.parse_args()
    dsn = os.environ.get("CHAT_DB_DSN")
    psql = shutil.which("psql")
    if not dsn or not psql:
        parser.error("requires CHAT_DB_DSN and system psql")
    temp = ROOT / ".temp"
    temp.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pg-recovery-", dir=temp) as directory:
        env = dict(os.environ, CHAT_CREDENTIALS="recovery:user:local-secret")
        server = Process([str(Path(args.server).resolve()), "--port", "0", "--threads", "4"],
                         env, Path(directory), "server")
        try:
            port = server.ready()
            with Peer(port) as peer:
                peer.hello("recovery", "user", "local-secret")
                message_id, nonce = send_message(peer, "persist through connection failure")
                connection, environment = psql_connection(dsn)
                subprocess.run([psql, *connection, "-X", "-v", "ON_ERROR_STOP=1", "-c",
                                "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                                "WHERE datname=current_database() AND usename=current_user "
                                "AND pid<>pg_backend_pid()"],
                               env=environment, check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                recovered = False
                for _ in range(12):
                    request = peer.send(5, ["lobby", "0", "100"])
                    while True:
                        frame = peer.receive()
                        if frame.request != request:
                            continue
                        if frame.kind == 11:
                            require(frame.fields[0] == "storage", "unexpected recovery error")
                            break
                        if frame.kind == 6:
                            recovered = True
                            break
                    if recovered:
                        break
                    time.sleep(.1)
                require(recovered, "PostgreSQL pool never recovered after backend termination")
                duplicate, _ = send_message(peer, "persist through connection failure", nonce=nonce)
                require(duplicate == message_id, "nonce idempotency lost during recovery")
                send_message(peer, "new write after reconnect")
            print("PASS PostgreSQL: terminated pool connections recover without chat-server restart; no write auto-replay")
        finally:
            server.close()


if __name__ == "__main__":
    main()
