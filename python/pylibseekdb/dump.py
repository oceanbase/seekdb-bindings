"""Command-line entry point for seekdb-dump."""

from ._migration import dump_main


def main() -> int:
    return dump_main()


if __name__ == "__main__":
    raise SystemExit(main())
