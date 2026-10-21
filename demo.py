"""Start the isolated Windows / WSL three-account demonstration."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "scripts"))
import hybrid


def setup():
    if os.name != "nt":
        raise RuntimeError("Run these launchers with Windows Python")
    hybrid.select_profile("demo")


def stop_frontends():
    runtime = hybrid.read_runtime()
    for entry in runtime.get("frontends", []):
        image = hybrid.windows_image(entry["pid"])
        if image is None:
            continue
        if image.resolve() != hybrid.client_path().resolve():
            raise RuntimeError("Frontend PID was reused; refusing to stop it")
        if hybrid.windows_started(entry["pid"]) != entry["started"]:
            raise RuntimeError("Frontend PID was reused; refusing to stop it")
        subprocess.run(["taskkill", "/PID", str(entry["pid"]), "/F"], check=True,
                       stdout=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
    runtime.pop("frontends", None)
    hybrid.write_json(hybrid.STATE / "runtime.json", runtime)


def stop():
    if not hybrid.STATE.exists():
        print("Demo is already stopped.")
        return
    hybrid.secure_directory(hybrid.STATE)
    stop_frontends()
    for user in hybrid.USERS:
        hybrid.stop_daemon(user)
    if (hybrid.STATE / "config.json").exists():
        hybrid.stop_backend()
    print("Demo stopped. Database and chat history retained; PostgreSQL remains running.")


def open_frontends():
    stop_frontends()
    for user in hybrid.USERS:
        process = subprocess.Popen([str(hybrid.client_path()), "--state-dir",
                                    str(hybrid.daemon_state(user))], cwd=ROOT,
                                   creationflags=subprocess.CREATE_NEW_CONSOLE)
        runtime = hybrid.read_runtime()
        runtime.setdefault("frontends", []).append({"pid": process.pid, "user": user,
                                                   "started": hybrid.windows_started(process.pid)})
        hybrid.write_json(hybrid.STATE / "runtime.json", runtime)


def admin_sql(sql):
    result = subprocess.run(["wsl.exe", "-u", "postgres", "--", "psql", "-X", "-qAt",
                             "-v", "ON_ERROR_STOP=1", "-d", "postgres"],
                            input=sql, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=40,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise RuntimeError(result.stderr[-1500:])
    return result.stdout.strip()


def cleanup():
    if not hybrid.STATE.exists():
        print("Nothing to clean.")
        return
    hybrid.secure_directory(hybrid.STATE)
    config = hybrid.read_json(hybrid.STATE / "config.json")
    if not config:
        raise RuntimeError("Missing demo configuration; refusing destructive cleanup")
    name, marker = config.get("database"), config.get("marker", "")
    if name != "socket_chat_demo" or not re.fullmatch(r"[0-9a-f]{64}", marker):
        raise RuntimeError("Invalid demo ownership record; refusing cleanup")
    stop()
    # Verify both ownership markers before issuing either destructive statement.
    sql = f"""
DO $$ BEGIN
IF EXISTS (SELECT FROM pg_database WHERE datname='{name}'
           AND (datdba IS DISTINCT FROM (SELECT oid FROM pg_roles WHERE rolname='{name}')
                OR shobj_description(oid, 'pg_database') IS DISTINCT FROM '{marker}'))
OR EXISTS (SELECT FROM pg_roles WHERE rolname='{name}'
           AND shobj_description(oid, 'pg_authid') IS DISTINCT FROM '{marker}') THEN
RAISE EXCEPTION 'Demo ownership mismatch; refusing cleanup';
END IF;
END $$;
DROP DATABASE IF EXISTS {name};
DROP ROLE IF EXISTS {name};
"""
    target = hybrid.STATE
    if target.resolve() != (ROOT / ".cache/showcase").absolute():
        raise RuntimeError("Demo state resolved outside its expected directory")
    if any(p.is_symlink() or getattr(p.lstat(), "st_file_attributes", 0)
           & stat.FILE_ATTRIBUTE_REPARSE_POINT
           for p in target.rglob("*")):
        raise RuntimeError("Linked demo state found; refusing recursive removal")
    admin_sql(sql)
    shutil.rmtree(target)
    print("Deleted only the demo database, role, credentials, logs and account caches.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-open", action="store_true", help="Start without opening consoles")
    parser.add_argument("--verify", action="store_true", help="Run bidirectional persistence/reconnect journey")
    args = parser.parse_args()
    setup()
    hybrid.start()
    if args.verify:
        sys.path.insert(0, str(ROOT / "tests"))
        import hybrid_journey
        hybrid_journey.main()
    if not args.no_open:
        open_frontends()
    print("Alice, Bob and Charlie share the lobby. Type a message; /read marks read; /quit closes one TUI.")
    print("Stop: python stopdemo.py | Delete demo data: python cleanupdemo.py")


def run(action):
    try:
        action()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"demo: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    run(main)
