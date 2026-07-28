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

One process can manage multiple local seekdb runtimes by selecting each
instance with its database directory:

```python
import pylibseekdb as seekdb

seekdb.open("./first.db")
seekdb.open("./second.db")

first = seekdb.connect(database="test", db_dir="./first.db")
second = seekdb.connect(database="test", db_dir="./second.db")

first.close()
second.close()
seekdb.close("./first.db")
seekdb.close("./second.db")
```

Each instance uses the local socket inside its normalized database directory,
so no additional port configuration is needed. `open()` is idempotent per
normalized database path. When exactly one instance is open, `connect()` and
`connection_options()` may omit `db_dir` for backward compatibility. When
multiple instances are open, they require `db_dir` to avoid selecting the wrong
runtime. `close(db_dir)` releases one lifecycle handle; plain `close()` releases
all open handles.

### Connect with PyMySQL

`connection_options()` returns endpoint and authentication arguments
shared by Python MySQL-protocol drivers. Install the driver separately:

```bash
pip install PyMySQL
```

```python
import pymysql
import pylibseekdb as seekdb

seekdb.open(db_dir="./seekdb.db")
options = seekdb.connection_options()

connection = pymysql.connect(database="test", **options)
try:
    with connection.cursor() as cursor:
        cursor.execute("SELECT 1")
        print(cursor.fetchone())
finally:
    # External connections must release the server before its lifecycle handle.
    connection.close()
    seekdb.close()
```

On Unix, `options` contains only `user="root"` and `unix_socket`. For TCP it
contains only `user="root"` and `port`; the driver supplies its default local
host. The database name remains caller-owned because PyMySQL uses `database`
while aiomysql uses `db`. Treat the returned dictionary as lifecycle-scoped:
do not use it after `seekdb.close()`.

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
    await seekdb.aopen(db_dir="./seekdb.db")
    options = seekdb.connection_options()
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
        seekdb.close()


asyncio.run(main())
```

`aopen()` runs the synchronous C startup operation in a worker thread so it
does not block the asyncio event loop. Cancelling the coroutine cannot stop
`seekdb_open()` after that worker starts; the runtime may finish opening and
must still be released with `close()`.

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
| `open(db_dir="./seekdb.db")` | Start a local seekdb runtime for the given database directory. Must be called before `connect()`. |
| `await aopen(db_dir="./seekdb.db")` | Run `open()` in a worker thread without blocking the asyncio event loop. |
| `connection_options(db_dir=None)` | Return a new dictionary for PyMySQL or aiomysql. Select `db_dir` when multiple instances are open. The database name is not included. |
| `connect(database="test", autocommit=False, db_dir=None)` | Return a `Connection` to the selected seekdb instance and database. |
| `close(db_dir=None)` | Synchronously release one instance by path, or all instances when omitted. Idempotent. Close external connections and pools first. |

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
