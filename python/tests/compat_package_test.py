# -*- coding: utf-8 -*-
"""Check the final pure-Python pylibseekdb compatibility package contract."""

import importlib
import pathlib
import sys
import warnings

import seekdb


def test_compatibility_import():
    compat_src = (
        pathlib.Path(__file__).resolve().parents[1]
        / "compat"
        / "pylibseekdb"
        / "src"
    )
    sys.path.insert(0, str(compat_src))
    try:
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            legacy = importlib.import_module("pylibseekdb")

        assert len(caught) == 1
        assert caught[0].category is DeprecationWarning
        assert "renamed to seekdb" in str(caught[0].message)
        assert legacy.SeekdbError is seekdb.SeekdbError
        assert legacy.SeekdbInstance is seekdb.SeekdbInstance
        assert legacy.Connection is seekdb.Connection
        assert legacy.Cursor is seekdb.Cursor
        assert legacy.open is seekdb.open
        assert legacy.__version__ == seekdb.__version__
    finally:
        sys.path.remove(str(compat_src))
        sys.modules.pop("pylibseekdb", None)


if __name__ == "__main__":
    test_compatibility_import()
