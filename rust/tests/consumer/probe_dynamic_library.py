"""Open and close seekdb through a deployed Rust cdylib."""

import argparse
import ctypes
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("database", type=Path)
    args = parser.parse_args()

    library = ctypes.CDLL(str(args.library.resolve()))
    library.seekdb_consumer_probe.argtypes = [ctypes.c_char_p]
    library.seekdb_consumer_probe.restype = ctypes.c_int
    result = library.seekdb_consumer_probe(str(args.database.resolve()).encode())
    if result != 0:
        raise SystemExit(f"seekdb_consumer_probe failed with code {result}")
    print("Deployed Rust cdylib: queried SQL and closed seekdb")


if __name__ == "__main__":
    main()
