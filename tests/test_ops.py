"""Properties for operational connection-string handling (no database needed)."""

from pathlib import Path
import random
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from pg_tools import connection_options, psql_connection


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
