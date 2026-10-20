"""Stop only the demo applications, retaining PostgreSQL and chat history."""
from demo import run, setup, stop


def main():
    setup()
    stop()


if __name__ == "__main__":
    run(main)
