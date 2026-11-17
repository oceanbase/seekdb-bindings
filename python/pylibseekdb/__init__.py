"""Python bindings for the seekdb C client library.

The C-level extension lives at `pylibseekdb.pylibseekdb` (the .so file inside
this package). All its public names are re-exported here so users can do:

    import pylibseekdb as seekdb
    seekdb.open(db_dir="./seekdb.db")
"""
import asyncio as _asyncio

from .pylibseekdb import *  # noqa: F401, F403

# DB-API 2.0 (PEP 249) exception classes. Re-export them explicitly so callers
# can `except pylibseekdb.SeekdbError` / `except pylibseekdb.Error` even if
# `import *` skips them. SeekdbError derives from DatabaseError and RuntimeError.
from .pylibseekdb import (  # noqa: F401
    DatabaseError,
    DataError,
    Error,
    IntegrityError,
    InterfaceError,
    InternalError,
    NotSupportedError,
    OperationalError,
    ProgrammingError,
    SeekdbError,
    Warning,
)


async def aopen(db_dir="./seekdb.db"):
    """Open and return a SeekdbInstance without blocking the event loop.

    Keep the returned instance and call instance.close() when finished.
    Module-level close() only closes the default instance.

    Cancelling this coroutine stops waiting but cannot stop seekdb_open() after
    its worker thread has started. Avoid cancellation when deterministic
    ownership and cleanup are required.
    """
    return await _asyncio.to_thread(open, db_dir)
