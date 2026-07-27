# -*- coding: utf-8 -*-
import asyncio
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


async def test_connection_options():
    assert_options_unavailable()

    db_dir = tempfile.mkdtemp(prefix="pylibseekdb-connection-options-")
    stop_ticker = asyncio.Event()
    ticks = 0

    async def ticker():
        nonlocal ticks
        while not stop_ticker.is_set():
            ticks += 1
            await asyncio.sleep(0.01)

    ticker_task = asyncio.create_task(ticker())
    try:
        await asyncio.gather(seekdb.aopen(db_dir), seekdb.aopen(db_dir))
    finally:
        stop_ticker.set()
        await ticker_task

    assert ticks > 1, "aopen() blocked the asyncio event loop"
    options = seekdb.connection_options()
    assert options == {
        "user": "root",
        "unix_socket": f"{db_dir}/run/sql.sock",
    }

    try:
        run_native_smoke_test()

        sync_connection = pymysql.connect(database="test", **options)
        try:
            with sync_connection.cursor() as cursor:
                cursor.execute("SELECT 1")
                assert cursor.fetchone() == (1,)
        finally:
            sync_connection.close()

        pool = await aiomysql.create_pool(db="test", minsize=1, maxsize=1, **options)
        try:
            async with pool.acquire() as async_connection:
                async with async_connection.cursor() as cursor:
                    await cursor.execute("SELECT 1")
                    assert await cursor.fetchone() == (1,)
        finally:
            pool.close()
            await pool.wait_closed()
    finally:
        seekdb.close()

    seekdb.close()
    assert_options_unavailable()


if __name__ == "__main__":
    asyncio.run(test_connection_options())
