"""Keep the controlling PTY session alive until child terminal modes are captured."""
import json
import os
import signal
import subprocess
import sys


def snapshot(fd):
    import termios
    attributes = termios.tcgetattr(fd)
    return attributes[:6] + [[value[0] if isinstance(value, bytes) else value
                             for value in attributes[6]]]


def main():
    report_fd = int(sys.argv[1])
    # Like a shell, remain alive if Ctrl+C reaches the foreground process group.
    # Caught dispositions reset to the default when the child is exec'd.
    signal.signal(signal.SIGINT, lambda *_: None)
    signal.signal(signal.SIGQUIT, lambda *_: None)
    report = {}
    try:
        report["before"] = snapshot(0)
        child = subprocess.Popen(sys.argv[2:])
        report["returncode"] = child.wait()
        report["after"] = snapshot(0)
    except BaseException as error:
        report["error"] = repr(error)
    finally:
        os.write(report_fd, json.dumps(report).encode("utf-8"))
        os.close(report_fd)
    code = report.get("returncode", 1)
    return code if code >= 0 else 128 - code


if __name__ == "__main__":
    sys.exit(main())
