"""Operational connection-string and demo-launcher tests (no database needed)."""

from pathlib import Path
import random
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from pg_tools import connection_options, psql_connection
import hybrid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import demo


class DemoTests(unittest.TestCase):
    def setUp(self):
        self.profile = hybrid.PROFILE
        hybrid.select_profile("demo")
        self.addCleanup(hybrid.select_profile, self.profile)

    def test_profile_accounts(self):
        self.assertEqual(hybrid.USERS, ("alice", "bob", "charlie"))
        hybrid.select_profile("hybrid")
        self.assertEqual(hybrid.USERS, ("alice", "bob"))

    def test_existing_demo_upgrade_is_retained_and_idempotent(self):
        root = Path(__file__).resolve().parents[1] / ".temp"
        root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as directory:
            with patch.object(hybrid, "STATE", Path(directory)), patch.object(hybrid, "stop_backend") as stop:
                original = {"users": {"alice": "existing-a", "bob": "existing-b"},
                            "dsn": "existing-dsn", "marker": "existing-marker"}
                hybrid.write_json(hybrid.STATE / "config.json", original)
                upgraded = hybrid.provision()
                stop.assert_called_once_with()
                self.assertEqual(upgraded["users"]["alice"], "existing-a")
                self.assertEqual(upgraded["users"]["bob"], "existing-b")
                self.assertEqual(len(upgraded["users"]["charlie"]), 64)
                self.assertEqual(upgraded["dsn"], original["dsn"])
                self.assertEqual(upgraded["marker"], original["marker"])
                self.assertEqual(hybrid.provision(), upgraded)
                stop.assert_called_once_with()

    def test_failed_backend_stop_leaves_credentials_unchanged(self):
        original = {"users": {"alice": "a", "bob": "b"}}
        with patch.object(hybrid, "read_json", return_value=original), \
                patch.object(hybrid, "stop_backend", side_effect=RuntimeError("stop failed")), \
                patch.object(hybrid, "write_json") as write:
            with self.assertRaises(RuntimeError):
                hybrid.provision()
            write.assert_not_called()
            self.assertNotIn("charlie", original["users"])

    def test_opens_and_tracks_three_distinct_frontends(self):
        runtime = {}
        with patch.object(demo, "stop_frontends") as stop, \
                patch.object(demo.subprocess, "CREATE_NEW_CONSOLE", 16, create=True), \
                patch.object(hybrid, "read_runtime", side_effect=lambda: runtime), \
                patch.object(hybrid, "write_json") as write, \
                patch.object(hybrid, "windows_started", return_value=123), \
                patch.object(demo.subprocess, "Popen") as launch:
            launch.return_value.pid = 42
            demo.open_frontends()
            stop.assert_called_once_with()
            self.assertEqual(launch.call_count, 3)
            self.assertEqual(write.call_count, 3)
            self.assertEqual([entry["user"] for entry in runtime["frontends"]], list(hybrid.USERS))
            for call, user in zip(launch.call_args_list, hybrid.USERS):
                self.assertEqual(call.args[0][-1], str(hybrid.daemon_state(user)))


class ConnectionTests(unittest.TestCase):
    def test_seeded_conninfo_roundtrip(self):
        rng = random.Random(2813)
        alphabet = "abc'\\ =\t中文012"
        def quote(value):
            return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"
        for _ in range(1000):
            values = {key: "".join(rng.choice(alphabet) for _ in range(rng.randrange(50)))
                      for key in ("host", "user", "password", "dbname")}
            dsn = " ".join(f"{key} = {quote(value)}" for key, value in values.items())
            self.assertEqual(connection_options(dsn), values)
            argv, env = psql_connection(dsn)
            self.assertEqual(env["PGPASSWORD"], values.pop("password"))
            self.assertEqual(argv[0], "--dbname")
            self.assertEqual(connection_options(argv[1]), values)

    def test_uri_password_is_not_in_arguments(self):
        argv, env = psql_connection("postgresql://alice:a%27b%40c@localhost:5432/chat?sslmode=require")
        self.assertEqual(env["PGPASSWORD"], "a'b@c")
        self.assertEqual(argv[1], "postgresql://alice@localhost:5432/chat?sslmode=require")

    def test_reject_invalid_input(self):
        for dsn in ("host", "=x", "host='never closed", "host='x'junk", "user=trailing\\"):
            with self.assertRaises(ValueError, msg=dsn):
                connection_options(dsn)
        with self.assertRaises(ValueError):
            psql_connection("postgres://u@localhost/db?password=secret")


if __name__ == "__main__":
    unittest.main()
