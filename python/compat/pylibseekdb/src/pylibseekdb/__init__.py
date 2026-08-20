"""Deprecated compatibility import for the renamed :mod:`seekdb` package."""

import warnings as _warnings

_warnings.warn(
    "pylibseekdb was renamed to seekdb; use `import seekdb` instead",
    DeprecationWarning,
    stacklevel=2,
)

from seekdb import *  # noqa: E402,F401,F403
from seekdb import SeekdbError, __version__  # noqa: E402,F401
