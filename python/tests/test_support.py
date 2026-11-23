"""Temporary directories for tests that start an embedded engine."""

from contextlib import contextmanager
import os
import tempfile
import time


@contextmanager
def temporary_directory(**kwargs):
    directory = tempfile.TemporaryDirectory(**kwargs)
    try:
        yield directory.name
    finally:
        # Closing the last client starts an asynchronous engine shutdown.
        # Windows cannot delete the copied executable until that process exits.
        deadline = time.monotonic() + 30
        while True:
            try:
                directory.cleanup()
                break
            except PermissionError:
                if os.name != "nt" or time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)
