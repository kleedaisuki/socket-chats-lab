"""Provision an isolated PostgreSQL database and save a local server DSN.

Requires a PostgreSQL administrator connection in CHAT_ADMIN_DSN and the system
psql executable. Existing roles/databases are never taken over or overwritten.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys

from pg_tools import psql_connection

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", default="socket_chat_lab")
    parser.add_argument("--user", default="socket_chat_lab")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5432)
    args = parser.parse_args()
    for value in (args.database, args.user):
        if not re.fullmatch(r"[a-z][a-z0-9_]{0,62}", value):
            parser.error("database and user must be lowercase PostgreSQL identifiers")
    if not re.fullmatch(r"[A-Za-z0-9.:-]+", args.host) or not 1 <= args.port <= 65535:
        parser.error("invalid host/port")
    admin = os.environ.get("CHAT_ADMIN_DSN")
    if not admin:
        parser.error("set CHAT_ADMIN_DSN to your PostgreSQL administrator connection string")
    psql = shutil.which("psql")
    if not psql:
        parser.error("psql not found; install PostgreSQL client tools")
    directory = ROOT / ".cache" / "database"
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    directory.chmod(0o700)
    state = directory / f"{args.database}.json"
    if state.exists():
        parser.error(f"{state} exists; refusing to replace credentials")
    password = secrets.token_hex(32)
    # psql variables are quoted as identifiers/SQL literals by psql itself.
    # Secrets travel on stdin/environment, never the process argument list.
    sql = (f"\\set role {args.user}\n\\set database {args.database}\n\\set password {password}\n"
           "CREATE ROLE :\"role\" LOGIN PASSWORD :'password';\n"
           "CREATE DATABASE :\"database\" OWNER :\"role\" ENCODING 'UTF8' TEMPLATE template0;\n")
    connection, environment = psql_connection(admin)
    result = subprocess.run([psql, *connection, "-X", "-v", "ON_ERROR_STOP=1", "-f", "-"],
                            input=sql, text=True, encoding="utf-8", env=environment,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if result.returncode:
        # PostgreSQL errors can quote statements containing a password.
        print(result.stderr.replace(password, "[redacted]"), file=sys.stderr)
        print("Provisioning stopped. If the role was created but database creation failed, remove that new role before retrying.", file=sys.stderr)
        return 1
    dsn = f"host={args.host} port={args.port} dbname={args.database} user={args.user} password={password}"
    descriptor = os.open(state, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
        json.dump({"dsn": dsn, "database": args.database, "user": args.user}, output, indent=2)
        output.write("\n")
    print(f"Created database. Sensitive connection configuration: {state}")
    print("Load its 'dsn' into CHAT_DB_DSN before starting chat-server. Do not commit this file.")
    if os.name == "nt":
        print("WARNING: provisioner output inherits Windows ACLs; restrict this directory to your account before production use.", file=sys.stderr)
    elif state.stat().st_mode & 0o077:
        print("WARNING: filesystem does not enforce POSIX modes; protect this directory with host ACLs.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
