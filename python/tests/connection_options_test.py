# -*- coding: utf-8 -*-
import asyncio
import os
import pathlib
import tempfile

import aiomysql
import pymysql
import pylibseekdb as seekdb
from seekdb_test import run_native_smoke_test


def assert_options_unavailable():
    try:
        seekdb.connection_options()
    except RuntimeError:
        return
    raise AssertionError("connection_options() succeeded without an open seekdb")


def assert_instance_closed(instance):
    for operation in (
        instance.connection_options,
        lambda: instance.connect("test"),
    ):
        try:
            operation()
        except RuntimeError as error:
            assert "closed" in str(error)
        else:
            raise AssertionError("operation succeeded on a closed seekdb instance")


def assert_tcp_options(options):
    assert set(options) == {"host", "port", "user"}
    assert options["user"] == "root"
    assert options["host"] == "127.0.0.1"
    assert 1 <= options["port"] <= 65535


def assert_unix_socket_options(options, db_dir):
    assert set(options) == {"unix_socket", "user"}
    assert options["user"] == "root"
    endpoint = pathlib.Path(options["unix_socket"])
    alias_dir = endpoint.parent.parent
    name_prefix = f"pylibseekdb-uds-{os.getpid()}-"

    assert alias_dir.parent == pathlib.Path("/tmp")
    assert alias_dir.name.startswith(name_prefix)
    assert len(alias_dir.name) == len(name_prefix) + 6
    assert endpoint == alias_dir / "run" / "sql.sock"
    assert alias_dir.is_dir()
    assert endpoint.parent.is_symlink()
    assert endpoint.parent.resolve() == (pathlib.Path(db_dir) / "run").resolve()
    assert endpoint.exists()
    return alias_dir


def assert_platform_options(options, db_dir):
    if os.name == "nt":
        assert_tcp_options(options)
        return None
    return assert_unix_socket_options(options, db_dir)


def write_marker(instance, value):
    connection = instance.connect("test", autocommit=True)
    cursor = connection.cursor()
    try:
        cursor.execute("create table instance_marker(value int)")
        cursor.execute(f"insert into instance_marker values ({value})")
    finally:
        cursor.close()
        connection.close()


def read_marker(instance):
    connection = instance.connect("test")
    cursor = connection.cursor()
    try:
        cursor.execute("select value from instance_marker")
        return cursor.fetchone()[0]
    finally:
        cursor.close()
        connection.close()


async def test_connection_options():
    assert_options_unavailable()

    with tempfile.TemporaryDirectory(prefix="pylibseekdb-multiple-instances-") as root_dir:
        long_root = pathlib.Path(root_dir)
        for index in range(4):
            long_root /= f"pylibseekdb-long-path-component-{index}"
        long_root.mkdir(parents=True)

        first_db_dir = str(long_root / "first")
        first_open_dir = str(long_root / "not-created" / ".." / "first")
        second_db_dir = str(long_root / "second")
        legacy_db_dir = str(pathlib.Path(root_dir) / "legacy")
        assert len(str(pathlib.Path(first_db_dir) / "run" / "sql.sock")) > 150

        stop_ticker = asyncio.Event()
        ticks = 0
        first = None
        duplicate = None
        second = None
        legacy = None
        first_alias_dir = None
        second_alias_dir = None
        legacy_alias_dir = None

        async def ticker():
            nonlocal ticks
            while not stop_ticker.is_set():
                ticks += 1
                await asyncio.sleep(0.01)

        ticker_task = asyncio.create_task(ticker())
        try:
            try:
                first = await seekdb.aopen(first_open_dir)
                duplicate, second = await asyncio.gather(
                    seekdb.aopen(first_db_dir),
                    seekdb.aopen(second_db_dir),
                )
            finally:
                stop_ticker.set()
                await ticker_task

            assert ticks > 1, "aopen() blocked the asyncio event loop"
            assert isinstance(first, seekdb.SeekdbInstance)
            assert isinstance(second, seekdb.SeekdbInstance)
            assert first.db_dir == first_db_dir
            assert duplicate.db_dir == first_db_dir
            assert second.db_dir == second_db_dir
            assert not first.closed
            assert not duplicate.closed
            assert not second.closed

            first_options = first.connection_options()
            first_alias_dir = assert_platform_options(first_options, first_db_dir)
            assert duplicate.connection_options() == first_options
            assert seekdb.connection_options() == first_options

            duplicate.close()
            assert duplicate.closed
            assert not first.closed
            if first_alias_dir is not None:
                assert first_alias_dir.is_dir()

            second_options = second.connection_options()
            second_alias_dir = assert_platform_options(second_options, second_db_dir)
            if os.name == "nt":
                assert second_options["port"] != first_options["port"]
            else:
                assert second_alias_dir != first_alias_dir
            assert seekdb.connection_options() == first_options

            write_marker(first, 1)
            write_marker(second, 2)
            assert read_marker(first) == 1
            assert read_marker(second) == 2

            run_native_smoke_test(first)

            sync_connection = pymysql.connect(database="test", **first_options)
            try:
                with sync_connection.cursor() as cursor:
                    cursor.execute("SELECT 1")
                    assert cursor.fetchone() == (1,)
                    cursor.execute("SELECT mysql_port()")
                    expected_port = first_options["port"] if os.name == "nt" else 0
                    assert cursor.fetchone() == (expected_port,)
                    cursor.execute(
                        "SELECT SQL_PORT FROM oceanbase.V$OB_SERVER_STAT "
                        "WHERE START_SERVICE_TIME > 0 LIMIT 1"
                    )
                    assert cursor.fetchone() == (expected_port,)
            finally:
                sync_connection.close()

            pool = await aiomysql.create_pool(
                db="test", minsize=1, maxsize=1, **first_options
            )
            try:
                async with pool.acquire() as async_connection:
                    async with async_connection.cursor() as cursor:
                        await cursor.execute("SELECT 1")
                        assert await cursor.fetchone() == (1,)
            finally:
                pool.close()
                await pool.wait_closed()

            retained_connection = first.connect("test")
            retained_cursor = retained_connection.cursor()
            first.close()
            assert first.closed
            assert_instance_closed(first)
            assert_options_unavailable()
            if first_alias_dir is not None:
                assert first_alias_dir.is_dir()
            try:
                retained_cursor.execute("select value from instance_marker")
                assert retained_cursor.fetchone() == (1,)
            finally:
                retained_cursor.close()
                retained_connection.close()
            if first_alias_dir is not None:
                assert not first_alias_dir.exists()

            assert read_marker(second) == 2
            seekdb.close()
            assert not second.closed

            legacy = seekdb.open(legacy_db_dir)
            legacy_options = legacy.connection_options()
            legacy_alias_dir = assert_platform_options(legacy_options, legacy_db_dir)
            assert seekdb.connection_options() == legacy_options
            legacy_connection = seekdb.connect("test")
            legacy_connection.close()
            seekdb.close()
            assert legacy.closed
            if legacy_alias_dir is not None:
                assert not legacy_alias_dir.exists()
            assert read_marker(second) == 2

            second.close()
            assert_instance_closed(second)
            if second_alias_dir is not None:
                assert not second_alias_dir.exists()
        finally:
            seekdb.close()
            for instance in (legacy, second, duplicate, first):
                if instance is not None:
                    instance.close()

    seekdb.close()
    assert_options_unavailable()


if __name__ == "__main__":
    asyncio.run(test_connection_options())
