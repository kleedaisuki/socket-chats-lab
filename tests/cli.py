"""CLI help/error journeys must not start services or create account state."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    for name in ("server", "daemon", "client"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    env = {key: value for key, value in os.environ.items() if not key.startswith("CHAT_")}
    temporary = ROOT / ".temp"
    temporary.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cli-", dir=temporary) as directory:
        for name in ("server", "daemon", "client"):
            executable = str(Path(getattr(args, name)).resolve())
            help_result = subprocess.run([executable, "--help"], env=env, cwd=directory,
                                         capture_output=True, text=True, timeout=5)
            assert help_result.returncode == 0 and "chat" in help_result.stdout, help_result
            for invalid in (["--not-an-option"], ["--state-dir"] if name != "server" else ["--port"]):
                result = subprocess.run([executable, *invalid], env=env, cwd=directory,
                                        capture_output=True, text=True, timeout=5)
                assert result.returncode != 0 and result.stderr, result
        assert list(Path(directory).iterdir()) == [], "help/error created account state"
    print("PASS CLI help, unknown options, missing values, no configuration side effects")


if __name__ == "__main__":
    main()
