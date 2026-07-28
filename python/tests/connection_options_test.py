# -*- coding: utf-8 -*-
import asyncio
import pathlib
import tempfile

import aiomysql
import pymysql
import pylibseekdb as seekdb
from seekdb_test import run_native_smoke_test


def assert_options_unavailable(db_dir=None):
    try:
        seekdb.connection_options(db_dir)
    except RuntimeError:
        return
    raise AssertionError("connection_options() succeeded without an open seekdb")


def assert_instance_required():
    for operation in (
        seekdb.connection_options,
        lambda: seekdb.connect("test"),
    ):
        try:
            operation()
        except RuntimeError as error:
            assert "multiple seekdb instances" in str(error)
        else:
            raise AssertionError("operation succeeded without selecting a seekdb instance")


def write_marker(db_dir, value):
    connection = seekdb.connect("test", autocommit=True, db_dir=db_dir)
    cursor = connection.cursor()
    try:
        cursor.execute("create table instance_marker(value int)")
        cursor.execute(f"insert into instance_marker values ({value})")
    finally:
        cursor.close()
        connection.close()


def read_marker(db_dir):
    connection = seekdb.connect("test", db_dir=db_dir)
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

        stop_ticker = asyncio.Event()
        ticks = 0

        async def ticker():
            nonlocal ticks
            while not stop_ticker.is_set():
                ticks += 1
                await asyncio.sleep(0.01)

        ticker_task = asyncio.create_task(ticker())
        try:
            try:
                await asyncio.gather(
                    seekdb.aopen(first_open_dir),
                    seekdb.aopen(first_db_dir),
                    seekdb.aopen(second_db_dir),
                )
            finally:
                stop_ticker.set()
                await ticker_task

            assert ticks > 1, "aopen() blocked the asyncio event loop"
            assert_instance_required()

            first_options = seekdb.connection_options(first_db_dir)
            assert first_options == {
                "user": "root",
                "unix_socket": f"{first_db_dir}/run/sql.sock",
            }
            second_options = seekdb.connection_options(second_db_dir)
            assert second_options == {
                "user": "root",
                "unix_socket": f"{second_db_dir}/run/sql.sock",
            }

            write_marker(first_db_dir, 1)
            write_marker(second_db_dir, 2)
            assert read_marker(first_db_dir) == 1
            assert read_marker(second_db_dir) == 2

            run_native_smoke_test(first_db_dir)

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

            seekdb.close(first_db_dir)
            assert_options_unavailable(first_db_dir)
            assert seekdb.connection_options() == second_options
            assert read_marker(second_db_dir) == 2
        finally:
            seekdb.close()

    seekdb.close()
    assert_options_unavailable()


if __name__ == "__main__":
    asyncio.run(test_connection_options())
