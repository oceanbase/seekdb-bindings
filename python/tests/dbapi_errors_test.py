# -*- coding: utf-8 -*-
"""DB-API 2.0 (PEP 249) exception contract for pylibseekdb (issue #42)."""
import pickle
import tempfile

import pylibseekdb as seekdb

ER_NO_SUCH_TABLE = 1146

DATABASE_ERROR_SUBCLASSES = (
    "DataError",
    "OperationalError",
    "IntegrityError",
    "InternalError",
    "ProgrammingError",
    "NotSupportedError",
)


def assert_exception_hierarchy():
    for name in ("Warning", "Error", "InterfaceError", "DatabaseError", "SeekdbError"):
        cls = getattr(seekdb, name)
        assert cls.__module__ == "pylibseekdb", (name, cls.__module__)
        assert str(cls) == f"<class 'pylibseekdb.{name}'>", str(cls)
    for name in DATABASE_ERROR_SUBCLASSES:
        cls = getattr(seekdb, name)
        assert cls.__module__ == "pylibseekdb", (name, cls.__module__)
        assert issubclass(cls, seekdb.DatabaseError), name

    assert issubclass(seekdb.Warning, Exception)
    assert issubclass(seekdb.Error, Exception)
    assert not issubclass(seekdb.Warning, seekdb.Error)
    assert issubclass(seekdb.InterfaceError, seekdb.Error)
    assert issubclass(seekdb.DatabaseError, seekdb.Error)
    assert issubclass(seekdb.SeekdbError, seekdb.DatabaseError)
    # Kept for backward compatibility with `except RuntimeError`.
    assert issubclass(seekdb.SeekdbError, RuntimeError)


def assert_missing_table_error(instance):
    connection = instance.connect("test")
    cursor = connection.cursor()
    try:
        cursor.execute("drop table if exists dbapi_errors_missing_table")
        try:
            cursor.execute("DESCRIBE `test`.`dbapi_errors_missing_table`")
        except seekdb.Error as error:
            assert type(error) is seekdb.SeekdbError, type(error)
            assert isinstance(error, seekdb.DatabaseError)
            assert isinstance(error, RuntimeError)
            assert error.args[0] == ER_NO_SUCH_TABLE, error.args
            assert error.code == ER_NO_SUCH_TABLE
            assert error.errno == ER_NO_SUCH_TABLE
            assert isinstance(error.msg, str) and error.msg
            assert error.args == (ER_NO_SUCH_TABLE, error.msg)
            assert "dbapi_errors_missing_table" in error.msg

            restored = pickle.loads(pickle.dumps(error))
            assert type(restored) is seekdb.SeekdbError
            assert restored.args == error.args
            assert restored.code == ER_NO_SUCH_TABLE
        else:
            raise AssertionError("DESCRIBE of a missing table did not raise")

        # The connection stays usable after a reported error.
        cursor.execute("SELECT 1")
        assert cursor.fetchall() == [(1,)]
    finally:
        cursor.close()
        connection.close()


def main():
    assert_exception_hierarchy()
    with tempfile.TemporaryDirectory() as tmp:
        instance = seekdb.open(f"{tmp}/seekdb.db")
        try:
            assert_missing_table_error(instance)
        finally:
            instance.close()
    print("dbapi_errors_test passed")


if __name__ == "__main__":
    main()
