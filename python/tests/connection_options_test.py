# -*- coding: utf-8 -*-
import asyncio
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
        first_db_dir = str(pathlib.Path(root_dir) / "first")
        first_open_dir = str(pathlib.Path(root_dir) / "not-created" / ".." / "first")
        second_db_dir = str(pathlib.Path(root_dir) / "second")
        legacy_db_dir = str(pathlib.Path(root_dir) / "legacy")

        stop_ticker = asyncio.Event()
        ticks = 0
        first = None
        duplicate = None
        second = None
        legacy = None

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
            assert first_options == {
                "user": "root",
                "unix_socket": f"{first_db_dir}/run/sql.sock",
            }
            assert seekdb.connection_options() == first_options

            duplicate.close()
            assert duplicate.closed
            assert not first.closed

            second_options = second.connection_options()
            assert second_options == {
                "user": "root",
                "unix_socket": f"{second_db_dir}/run/sql.sock",
            }
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
            try:
                retained_cursor.execute("select value from instance_marker")
                assert retained_cursor.fetchone() == (1,)
            finally:
                retained_cursor.close()
                retained_connection.close()

            assert read_marker(second) == 2
            seekdb.close()
            assert not second.closed

            legacy = seekdb.open(legacy_db_dir)
            assert seekdb.connection_options() == legacy.connection_options()
            legacy_connection = seekdb.connect("test")
            legacy_connection.close()
            seekdb.close()
            assert legacy.closed
            assert read_marker(second) == 2

            second.close()
            assert_instance_closed(second)
        finally:
            seekdb.close()
            for instance in (legacy, second, duplicate, first):
                if instance is not None:
                    instance.close()

    seekdb.close()
    assert_options_unavailable()


if __name__ == "__main__":
    asyncio.run(test_connection_options())
