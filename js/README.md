# seekdb-js — JavaScript bindings

JavaScript bindings for the [seekdb](https://github.com/oceanbase/seekdb) C
client library, modeled on the Python bindings in `python/`. They link
`libseekdb` dynamically and ship the `seekdb` server binary inside the package,
so a local server process is started automatically on first open.

## Runtime support

The addon is a single N-API (node-addon-api) build shared across runtimes:

| Runtime  | Status            |
| -------- | ----------------- |
| Node.js  | yes (primary)     |
| Bun      | yes               |
| Deno     | yes (see below)   |
| Windows  | no                |

Requires Node.js >= 16 (N-API version 8). CI baseline: Node LTS 18 / 20 / 22.

Bun implements the Node-API interface, so the package loads as-is. Deno 2
loads npm packages with native addons when `deno.json` sets
`"nodeModulesDir": "auto"` (or `"manual"`) and the `--allow-ffi` permission
is granted.

## Build from source

Prerequisites: Node.js with `node-gyp` toolchain (Xcode CLT on macOS, `build-essential`
+ Python on Linux), and a prebuilt `libseekdb` + `seekdb` server binary.

```bash
cd js
npm install
```

`npm install` builds the addon via node-gyp. It links `libseekdb` from
`../build` by default; override with the `SEEKDB_LIB_DIR` environment variable:

```bash
SEEKDB_LIB_DIR=/path/to/seekdb/build npm install
```

The `seekdb` server binary is auto-discovered next to `libseekdb` at runtime,
so for development, copy both next to the built addon before running:

```bash
cp ../build/libseekdb.* ../build/seekdb build/Release/
```

## Usage

```js
const seekdb = require('seekdb-js');

// All blocking operations are Promise-based and run off the event loop
const instance = await seekdb.open('./seekdb.db');
const connection = await instance.connect('test');
const cursor = connection.cursor();
await cursor.execute('create table t(c1 int, c2 varchar(64))');
await cursor.execute('insert into t values (1, "hello")');
await connection.commit();
await cursor.execute('select c1, c2 from t');
const rows = await cursor.fetchAll();
console.log(rows); // [[1, "hello"]]
await instance.close();
```

The API surface mirrors the Python bindings (`open/close/connectionOptions/connect`,
`Connection.begin/commit/rollback/close`, `Cursor.execute/fetchOne/fetchAll/close`);
every blocking call returns a Promise. Only pure in-memory accessors
(`cursor()`, `closed`, `dbDir`) are synchronous.

### Value mapping

| SQL type           | JS value                                              |
| ------------------ | ----------------------------------------------------- |
| `NULL`             | `null`                                                |
| `INT64`/`UINT64`   | `number`, or `bigint` outside `[-(2^53-1), 2^53-1]`    |
| `FLOAT`            | `number`                                              |
| `DECIMAL`          | `string`                                              |
| `DATE`             | `"YYYY-MM-DD"`                                        |
| `DATETIME`/`TIMESTAMP` | `"YYYY-MM-DD HH:MM:SS.ffffff"`                    |
| `VARCHAR`          | `string`                                              |

### Errors

All failures reject/throw a `SeekdbError` (subclass of `Error`) carrying a
`code` property filled from `seekdb_last_error` (server errno) when available.

### Cursor concurrency

`execute`/`fetchOne`/`fetchAll` on the same cursor are serialized internally;
await them in order. The native result pointer is additionally mutex-protected.

## Tests

```bash
npm test
```

Tests require the `seekdb` server binary next to the addon (see layout above)
and use Node's built-in `node:test` runner.

## API

See `lib/seekdb.d.ts` for the complete TypeScript signatures.

- `open(dbDir?)` — open a local instance; the first open becomes the module
  default.
- `close()` — close the module-default instance.
- `connectionOptions()` — connection options of the module-default instance
  (`{user, port?, unix_socket?}`).
- `connect(database?, autocommit?)` — connect the module-default instance.
- `SeekdbInstance` — `connect`, `connectionOptions`, `close`, `closed`, `dbDir`.
- `Connection` — `cursor()`, `begin`, `commit`, `rollback`, `close`, `closed`.
- `Cursor` — `execute` (returns affected rows), `fetchOne`, `fetchAll`, `close`,
  `closed`.