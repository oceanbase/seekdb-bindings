# pylibseekdb

Low-level Python bindings for the [seekdb](https://github.com/oceanbase/seekdb) C client library.

## 🚀 What is OceanBase seekdb?

**OceanBase seekdb** is an AI-native search database that unifies relational, vector, full-text, JSON, and GIS in a single engine, enabling hybrid search and in-database AI workflows.

> 📖 [Read the launch blog →](https://github.com/oceanbase/seekdb/blob/develop/docs/blog/launch_blog_en.md) · 📚 [Docs →](https://docs.seekdb.ai/)

## ✨ Why seekdb for Agents?

### 🔥 Streaming Write + Concurrent Search, Without the P99 Spike

Agent workloads are continuous write + millisecond-later read. seekdb's **async index pipeline (Change Stream)** decouples DML from index build, and its **two-level HNSW** (incremental + snapshot) makes newly-written vectors immediately searchable.

<div align="center">
  <img src="https://raw.githubusercontent.com/oceanbase/seekdb/refs/heads/develop/images/architecture.svg" alt="seekdb async index pipeline architecture" width="720" />
</div>

The write path commits and returns _without waiting_ on index construction. The Change Stream pipeline consumes the redo log asynchronously and updates the delta HNSW. Queries hit both delta and snapshot indexes with fine-grained read locks — **this is why P99 stays flat under concurrency.**

### 🌿 Copy-on-Write Sandboxes for Agent Exploration

`FORK DATABASE` snapshots an entire database in seconds — no data copy. Agents experiment freely (write, query, even break tables); then `MERGE TABLE` commits the work back, or `DROP DATABASE` discards it.

### 🔍 Hybrid Search in a Single SQL

Vector + full-text + scalar filter pushed into one execution plan. No N+1 client-side merging, no glue code to combine results.

### 🐬 MySQL-Compatible, ACID, Embeddable

Built on the proven OceanBase SQL engine. Works as an embedded library, a single-node server, or in the OceanBase distributed cluster. Full ACID, real-time writes, and the entire MySQL ecosystem out of the box.

## Installation

```bash
pip install pylibseekdb
```

## Logical version migration

`pylibseekdb` installs `seekdb-dump` and `seekdb-restore` for migrating an
embedded database without opening an old data directory with a new runtime.
Stop all application writes and DDL, then create the dump while the old wheel
is still installed:

```bash
seekdb-dump ./old.db -o backup.sql
```

After installing the new wheel, restore into a new or otherwise empty instance:

```bash
seekdb-restore ./new.db backup.sql
```

The dump is mysql-compatible SQL, so stdout, stdin, and external compression
can be used:

```bash
seekdb-dump ./old.db | gzip > backup.sql.gz
gzip -dc backup.sql.gz | seekdb-restore ./new.db
```

By default all user databases are included. Repeat `--database NAME` to select
specific databases. System databases, users, and grants are never exported.
Tables, their data and indexes, and ordinary views are supported. Triggers,
stored routines, events, materialized views, and unknown object types are
reported before any SQL is written and make the command fail. To deliberately
create a partial dump, use `--ignore-unsupported`. Restore warns with the
skipped-object list and continues without those objects.

`seekdb-restore` refuses a target containing any user table or view. A failed
restore can contain already-applied DDL, so discard that target and retry with
an empty instance.

### Requirements

- CPython >= 3.11
- Linux x86_64 or aarch64 with glibc >= 2.28 (Alpine / musl not supported yet)
- macOS arm64 >= 15.6

## 🎬 Quick Start

`pylibseekdb` exposes a lightweight DB-API 2-style interface directly over the seekdb C driver.
It currently starts a local seekdb runtime via `open()`. Native embedded-mode support will be released soon.

```python
import pylibseekdb as seekdb

# Start a local seekdb runtime (embedded-mode support will be released soon)
seekdb.open(db_dir="./seekdb.db")

# Get a connection and a cursor
conn   = seekdb.connect(database="test", autocommit=True)
cursor = conn.cursor()

# Create a table with a vector column and an HNSW index
cursor.execute("""
    CREATE TABLE IF NOT EXISTS articles (
        id        INT PRIMARY KEY,
        title     TEXT,
        embedding VECTOR(4),
        VECTOR INDEX idx_vec (embedding)
            WITH (DISTANCE=l2, TYPE=hnsw, LIB=vsag)
    ) ORGANIZATION = HEAP
""")

# Insert a row
cursor.execute(
    "INSERT INTO articles VALUES (1, 'Hello seekdb', '[0.1, 0.2, 0.3, 0.4]')"
)

# Hybrid / vector search
cursor.execute("""
    SELECT id, title,
           l2_distance(embedding, '[0.1, 0.2, 0.3, 0.4]') AS dist
    FROM articles
    ORDER BY dist APPROXIMATE
    LIMIT 5
""")
rows = cursor.fetchall()
for row in rows:
    print(row)

cursor.close()
conn.close()
seekdb.close()
```

### Multiple instances

One process can manage multiple local seekdb runtimes through the
`SeekdbInstance` objects returned by `open()`:

```python
import pylibseekdb as seekdb

first = seekdb.open("./first.db")
second = seekdb.open("./second.db")

first_connection = first.connect(database="test")
second_connection = second.connect(database="test")

first_connection.close()
second_connection.close()
first.close()
second.close()
```

Each instance stores its local socket inside the normalized database directory.
On macOS and Linux, pylibseekdb connects through a per-instance short alias under
`/tmp/pylibseekdb-uds-<pid>-XXXXXX`, so long database paths do not exceed the
Unix socket pathname limit. The first successful `open()` also becomes the
module's default instance, preserving the legacy
`seekdb.connect()`, `seekdb.connection_options()`, and `seekdb.close()` API.
Later calls return independent instance objects without changing that default.
Use the object methods for additional instances.

### Connect with PyMySQL

`connection_options()` returns endpoint and authentication arguments shared by
Python MySQL-protocol drivers. PyMySQL is installed with `pylibseekdb`:

```bash
pip install pylibseekdb
```

```python
import pymysql
import pylibseekdb as seekdb

instance = seekdb.open(db_dir="./seekdb.db")
options = instance.connection_options()

connection = pymysql.connect(database="test", **options)
try:
    with connection.cursor() as cursor:
        cursor.execute("SELECT 1")
        print(cursor.fetchone())
finally:
    # External connections must release the server before its lifecycle handle.
    connection.close()
    instance.close()
```

On Unix, `options` contains only `user="root"` and `unix_socket`. For TCP it
contains only `user="root"` and `port`; the driver supplies its default local
host. The database name remains caller-owned because PyMySQL uses `database`
while aiomysql uses `db`. Treat the returned dictionary as lifecycle-scoped:
the Unix socket alias is removed with the underlying lifecycle handle, so do
not use it after closing its `SeekdbInstance` and any retained native
connections.

### Async initialization and aiomysql

Install aiomysql separately:

```bash
pip install aiomysql
```

```python
import asyncio

import aiomysql
import pylibseekdb as seekdb


async def main():
    instance = await seekdb.aopen(db_dir="./seekdb.db")
    options = instance.connection_options()
    pool = await aiomysql.create_pool(
        db="test",
        minsize=1,
        maxsize=5,
        **options,
    )
    try:
        async with pool.acquire() as connection:
            async with connection.cursor() as cursor:
                await cursor.execute("SELECT 1")
                print(await cursor.fetchone())
    finally:
        pool.close()
        await pool.wait_closed()
        instance.close()


asyncio.run(main())
```

`aopen()` runs the synchronous C startup operation in a worker thread and
returns a `SeekdbInstance`, so it does not block the asyncio event loop.
Cancelling the coroutine cannot stop `seekdb_open()` after that worker starts.

### Transaction support

```python
conn = seekdb.connect(database="test", autocommit=False)
cursor = conn.cursor()
try:
    conn.begin()
    cursor.execute("INSERT INTO articles VALUES (2, 'Second', '[0.5,0.6,0.7,0.8]')")
    conn.commit()
except seekdb.SeekdbError:
    conn.rollback()
    raise
finally:
    cursor.close()
    conn.close()
```

### SQL — Hybrid Search

```sql
-- Create table with vector column, full-text index, and HNSW vector index
CREATE TABLE docs (
    id        INT PRIMARY KEY,
    title     TEXT,
    content   TEXT,
    embedding VECTOR(384),
    FULLTEXT INDEX idx_fts (content) WITH PARSER ik,
    VECTOR   INDEX idx_vec (embedding)
        WITH (DISTANCE=l2, TYPE=hnsw, LIB=vsag)
) ORGANIZATION = HEAP;

-- Hybrid search: vector similarity + full-text match in one query
SELECT id, title,
       l2_distance(embedding, '[0.12, 0.34, ...]') AS dist
FROM docs
WHERE MATCH(content) AGAINST('quarterly report')
ORDER BY dist APPROXIMATE
LIMIT 10;
```

## API Reference

### Module-level functions

| Function | Description |
|---|---|
| `open(db_dir="./seekdb.db")` | Start a local runtime and return its `SeekdbInstance`. The first open instance becomes the module default. |
| `await aopen(db_dir="./seekdb.db")` | Run `open()` in a worker thread and return its `SeekdbInstance`. |
| `connection_options()` | Return connection arguments for the default instance. The database name is not included. |
| `connect(database="test", autocommit=False)` | Return a `Connection` to the default instance. |
| `close()` | Close and clear the default instance. Idempotent. |

### `SeekdbInstance`

| Attribute or method | Description |
|---|---|
| `db_dir` | Normalized absolute database directory used by this instance. |
| `closed` | Whether this instance has been closed. |
| `connect(database="test", autocommit=False)` | Return a `Connection` to this instance. |
| `connection_options()` | Return connection arguments for PyMySQL or aiomysql. |
| `close()` | Release this instance. Existing native `Connection` objects keep the underlying lifecycle handle alive until they close. |

### `Connection`

| Method | Description |
|---|---|
| `cursor()` | Return a new `Cursor`. |
| `begin()` | Begin a transaction. |
| `commit()` | Commit the current transaction. |
| `rollback()` | Roll back the current transaction. |
| `close()` | Disconnect and release resources. |

### `Cursor`

| Method | Description |
|---|---|
| `execute(sql)` | Execute *sql*; returns the number of rows in the result set (0 for statements without a result set). |
| `fetchone()` | Return the next row as a `tuple`, or `None`. |
| `fetchall()` | Return all remaining rows as a list of `tuple`. |
| `close()` | Free the result set. |

### `SeekdbError`

Exception raised on driver errors.  Subclass of `RuntimeError`.

## 📚 Use Cases

- **🤖 Agentic AI** — streaming memory writes, millisecond-later vector retrieval, `FORK DATABASE` for safe exploration
- **📖 RAG & Knowledge Retrieval** — hybrid search across enterprise knowledge bases
- **🔍 Semantic Search** — embedding-based search for text, images, and other modalities
- **💻 AI-Assisted Coding** — semantic code search with multi-project isolation
- **📱 On-Device & Edge AI** — lightweight local deployments today, with embedded-mode support coming soon

## 🌐 Resources

- 📖 [Docs](https://docs.seekdb.ai/)
- 🐍 [pyseekdb (high-level SDK)](https://github.com/oceanbase/pyseekdb)
- 🐛 [Issues](https://github.com/oceanbase/seekdb/issues)
- 💬 [Discord](https://discord.gg/74cF8vbNEs)
- 🏗️ [seekdb-bindings (this repo)](https://github.com/oceanbase/seekdb-bindings)

## License

Apache-2.0 — see [LICENSE](https://github.com/oceanbase/seekdb-bindings/blob/main/LICENSE).
