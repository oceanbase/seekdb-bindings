"""Command-line entry point for seekdb-restore."""

from ._migration import restore_main


def main() -> int:
    return restore_main()


if __name__ == "__main__":
    raise SystemExit(main())
