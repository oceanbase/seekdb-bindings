"""Python bindings for the seekdb C client library.

The C-level extension lives at `pylibseekdb.pylibseekdb` (the .so file inside
this package). All its public names are re-exported here so users can do:

    import pylibseekdb as seekdb
    seekdb.open(db_dir="./seekdb.db")
"""
import asyncio as _asyncio
import os as _os
from pathlib import Path as _Path

_native_directory = _Path(__file__).parent / "bin"
if _os.name == "nt" and _native_directory.is_dir():
    # Keep the handle alive while the extension and its dependent DLLs are used.
    _native_dll_directory = _os.add_dll_directory(str(_native_directory))

from .pylibseekdb import *  # noqa: F401, F403

# pybind11 exception class — not always caught by `import *`, re-export
# explicitly so callers can `except pylibseekdb.SeekdbError`.
from .pylibseekdb import SeekdbError, __version__  # noqa: F401


async def aopen(db_dir="./seekdb.db"):
    """Open and return a SeekdbInstance without blocking the event loop.

    Keep the returned instance and call instance.close() when finished.
    Module-level close() only closes the default instance.

    Cancelling this coroutine stops waiting but cannot stop seekdb_open() after
    its worker thread has started. Avoid cancellation when deterministic
    ownership and cleanup are required.
    """
    return await _asyncio.to_thread(open, db_dir)
