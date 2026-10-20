"""Delete only this demo's database, role and local state. This erases demo chats."""
from demo import cleanup, run, setup


def main():
    setup()
    cleanup()


if __name__ == "__main__":
    run(main)
