# seekdb Node.js binding

This package manages a local seekdb runtime. It starts or reuses the seekdb
server, returns the local MySQL-protocol endpoint, and releases the lifecycle
handle. SQL connections are owned by the application and its chosen client.

The public API is written in TypeScript and is available to JavaScript and
TypeScript applications through one package. Node.js 22 or newer is the initial
supported runtime. The package uses ES modules (`import`). Bun and Deno have not
been validated with this package.

## Build from this repository

Initialize the repository's git submodules, install Node.js and CMake, and make
a compatible `seekdb` executable available on `PATH` or set `SEEKDB_BIN`:

```sh
git submodule update --init --recursive
cd typescript
npm install
SEEKDB_BIN=/path/to/seekdb npm test
```

`npm test` builds the package before running the tests. To build without testing,
run `SEEKDB_BIN=/path/to/seekdb npm run build`. The build creates `libseekdb`,
compiles the Node-API addon and TypeScript, then stages the addon, library, and
server executable under `dist/native`.
The native files must match the host operating system and architecture. Linux
builds additionally require a C/C++ toolchain, `readelf` (binutils), `ldd`, and
`patchelf`. Staging recursively bundles non-glibc shared libraries, rejects
missing or conflicting dependencies, and sets their runtime search paths to
`$ORIGIN`. glibc and its loader remain system dependencies.

`CC`, `CXX`, and `LDFLAGS` are preserved. On macOS, unset compilers are selected
with `xcrun --find clang` / `clang++`, respecting the active developer SDK. The
same compiler selection applies to the C library and addon. Use a fresh
`build/node-sdk` directory when changing a compiler already cached by CMake.

## Build a Linux x64 npm tarball

Build in a glibc 2.28 environment, such as an AlmaLinux/Rocky Linux 8 builder
with a current compiler toolchain and Node.js 22 or 24. The existing
`scripts/build-seekdb-glibc228.sh` can build the server executable. Build the C
library, addon, and any bundled dependencies against the same glibc baseline.
The ordinary Debian-based `node` images may introduce newer glibc requirements;
Alpine/musl is not a supported target of this command.

```sh
SEEKDB_BIN=/path/to/seekdb npm run build
npm run pack:linux -- --out-dir /path/to/artifacts
```

`pack:linux` checks the built files, dependency closure, executable permissions,
and every ELF's GLIBC symbol requirements, rejecting anything above 2.28. It
creates a publishable `@oceanbase/seekdb-node` tarball restricted to Linux/x64/glibc,
including the license and native runtime. The source manifest stays private;
the generated manifest has public npm publish settings and no build scripts or
development dependencies. No package is uploaded by this command.

For testing a different compatibility target, pass `--glibc-baseline <version>`.
`platform.json` records the highest required glibc symbol version. Choosing a
lower baseline cannot make an existing binary compatible; rebuild the offending
binary or dependency in the target environment. `npm pack` also validates the
built package, so a clean checkout cannot silently produce an empty tarball.

Before publishing, install the tarball outside the source tree and verify SQL
with Node.js 22 and 24 on the baseline distribution and a newer distribution.
Confirm publish rights to the chosen npm scope. The existing `seekdb` SDK uses
`@seekdb/js-bindings`, whose `open/connect/execute` API differs from this package's
lifecycle API. Adapting that SDK is required before replacing its native package.

## Use

```js
import { seekdb } from "@oceanbase/seekdb-node";
import mysql from "mysql2/promise";

const db = await seekdb.open("./seekdb.db", {
  parameters: { memory_budget: "2G" },
});
let connection;
try {
  const endpoint = db.connectionOptions();
  const location = endpoint.transport === "tcp"
    ? { host: endpoint.host, port: endpoint.port }
    : { socketPath: endpoint.transport === "unix_socket"
        ? endpoint.unixSocket : endpoint.namedPipe };
  connection = await mysql.createConnection({
    user: endpoint.user,
    password: "",
    database: "test",
    ...location,
  });
  const [rows] = await connection.query("SELECT 1 AS value");
  console.log(rows);
} finally {
  if (connection) await connection.end();
  db.close();
}
```

The endpoint is valid while its `seekdb` handle remains open. Close external SQL
connections before the handle. `close()` is idempotent and does not delete data
or guarantee immediate server-process termination. The returned `dbDir` is an
absolute, normalized path. On Windows, a named-pipe endpoint may require a
client-specific transport adapter; validate the chosen client before relying on
it. The package exposes connection information rather than a client-specific
configuration object.

Server parameters use the names and string values accepted by `seekdb_open`.
Ordinary parameters apply on first initialization; `port` and `mysql_port_mode`
apply on each spawn. Errors from `open()` include `returnCode` and the native
startup diagnostic. A native handle is also closed when garbage collected, but
applications should call `close()` explicitly.
