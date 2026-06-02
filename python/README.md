# pylibseekdb

Low-level Python bindings for the [seekdb](https://github.com/oceanbase/seekdb) C client library.

## 🚀 What is OceanBase seekdb?

**OceanBase seekdb** is an AI-native search database that unifies relational, vector, full-text, JSON, and GIS in a single engine, enabling hybrid search and in-database AI workflows.

> 📖 [Read the launch blog →](https://github.com/oceanbase/seekdb/blob/develop/docs/blog/launch_blog_en.md) · 📚 [Docs →](https://docs.seekdb.ai/)

## 🔥 Why OceanBase seekdb?

| Feature | seekdb | Chroma | Milvus | MySQL 8.0+ | PostgreSQL+pgvector | Elasticsearch |
|---|:---:|:---:|:---:|:---:|:---:|:---:|
| **Embedded** | ✅ | ✅ | ✅ | ❌ | ❌ | ❌ |
| **Single-Node** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **MySQL Compatible** | ✅ | ❌ | ❌ | ✅ | ❌ | ❌ |
| **Vector Search** | ✅ | ✅ | ✅ | ❌ | ✅ | ✅ |
| **Full-Text Search** | ✅ | ✅ | ⚠️ | ✅ | ✅ | ✅ |
| **Hybrid Search** | ✅ | ✅ | ✅ | ❌ | ⚠️ | ✅ |
| **OLTP** | ✅ | ❌ | ❌ | ✅ | ✅ | ❌ |
| **License** | Apache 2.0 | Apache 2.0 | Apache 2.0 | GPL 2.0 | PostgreSQL | AGPLv3 |

> ✅ Supported · ❌ Not Supported · ⚠️ Limited

## Installation

```bash
pip install pylibseekdb
```

### Requirements

- CPython >= 3.9
- Linux x86_64 with glibc >= 2.28 (Alpine / musl not supported yet)
- macOS arm64 >= 15.5
- Windows x86_64

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
```

### Transaction support

```python
conn = seekdb.connect(database="test", autocommit=False)
cursor = conn.cursor()
try:
    conn.begin()
    cursor.execute("INSERT INTO articles VALUES (2, 'Second', '[0.5,0.6,0.7,0.8]')")
    conn.commit()
except seekdb.SeekdbError as exc:
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
| `open(db_dir="./seekdb.db")` | Start a local seekdb runtime for the given database directory. Embedded-mode support will be released soon. Must be called before `connect()`. |
| `connect(database="test", autocommit=False)` | Return a `Connection` to the given database. |

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
