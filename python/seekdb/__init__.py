"""Python bindings for the seekdb C client library.

The C-level extension lives at `seekdb.seekdb_binding` (the .so file inside
this package). All its public names are re-exported here so users can do:

    import seekdb
    seekdb.open(db_dir="./seekdb.db")
"""
import asyncio as _asyncio
from importlib.metadata import PackageNotFoundError as _PackageNotFoundError
from importlib.metadata import version as _distribution_version

from .seekdb_binding import *  # noqa: F401, F403

# Native exception class — not always caught by `import *`, re-export
# explicitly so callers can `except seekdb.SeekdbError`.
from .seekdb_binding import SeekdbError  # noqa: F401

try:
    __version__ = _distribution_version("seekdb")
except _PackageNotFoundError:
    # A direct CMake build has no installed wheel metadata. Installed wheels
    # always take the version from their METADATA above.
    __version__ = "unknown"


async def aopen(db_dir="./seekdb.db"):
    """Open and return a SeekdbInstance without blocking the event loop.

    Keep the returned instance and call instance.close() when finished.
    Module-level close() only closes the default instance.

    Cancelling this coroutine stops waiting but cannot stop seekdb_open() after
    its worker thread has started. Avoid cancellation when deterministic
    ownership and cleanup are required.
    """
    return await _asyncio.to_thread(open, db_dir)
