# seekdb Rust binding 1.4.0

This crate starts or reuses a local seekdb instance, returns its MySQL-protocol
endpoint, and closes the lifecycle handle. Applications choose their own SQL
client. The examples use `mysql` and `mysql_async`; neither is a runtime
dependency of the binding.

## Build and run your first query

Run from this repository on Linux x86_64 with glibc 2.28 or newer:

```sh
cargo run --manifest-path rust/Cargo.toml --example first_query -- /tmp/seekdb-rust-demo
```

The example opens the database, connects through its Unix socket as `root` with
an empty password to database `test`, runs `SELECT 1`, and prints `SELECT 1 = 1`.
It closes the SQL connection before releasing the lifecycle handle. The complete
source is in [`examples/first_query.rs`](examples/first_query.rs).

To use the binding in your own application, add these dependencies to its
`Cargo.toml`, adjusting the checkout path:

```toml
[dependencies]
seekdb = { path = "../seekdb-bindings/rust" }
mysql = { version = "28", default-features = false, features = ["minimal-rust"] }
```

The crate is version `1.4.0` and remains `publish = false` while native platform
coverage is being validated, so it is not yet distributed by this project on
crates.io. Copy the example's connection setup into your application. On POSIX,
use `endpoint.unix_socket`; do not assume a TCP listener on port 3306. The
endpoint is valid only while its lifecycle handle remains open.

## Native runtime and Cargo configuration

On the first build, `build.rs` downloads the pinned seekdb 1.4.0.0 revision 1
native bundle from OSS, verifies its SHA-256, and extracts it into Cargo's
`OUT_DIR`. It reuses a valid cached bundle on later builds. Downloads honor
standard proxy variables and use Rust libraries for HTTP and extraction; no
external `curl` or `tar` command is required.

The build script reports the native library directory through Cargo's standard
`rustc-link-search` instruction. Cargo sets the dynamic loader search path for
`cargo run` and `cargo test`. No SeekDB-specific environment variables are used,
and runtime discovery does not embed an absolute build-output path.

Cargo's normal configuration applies, including `--target-dir`,
`CARGO_TARGET_DIR`, custom profiles, and `--target` for a supported native
target. For example:

```sh
CARGO_TARGET_DIR=/tmp/seekdb-target cargo test --manifest-path rust/Cargo.toml --locked
```

Only Linux x86_64 has an automatic bundle URL at present. macOS arm64 and Linux
arm64 builds will become available when their bundle URLs are configured;
Windows is not supported yet. The package helper does not cross-compile native
libraries or change their glibc requirements.

## Async applications

`Seekdb::open` waits synchronously for server readiness. A `Seekdb` handle is
neither `Send` nor `Sync`, so keep it on its owning thread. For Tokio, open,
hold, and close the handle inside `spawn_blocking`; do not return the handle
from that task. Pass the owned `ConnectionOptions` to an async MySQL client
instead.

[`examples/async_query.rs`](examples/async_query.rs) demonstrates this pattern
using `mysql_async`. It runs SQL on the Tokio runtime, disconnects the entire
connection pool, and then closes the lifecycle handle on its original thread:

```sh
cargo run --manifest-path rust/Cargo.toml --example async_query -- /tmp/seekdb-rust-async-demo
```

In a long-running service, keep the lifecycle task alive for the lifetime of
the SQL pools and disconnect those pools before signaling it to close. The
binding does not add an async runtime or implement cross-thread handle access.

## Parameters and lifecycle

Use `Seekdb::open_with_options(path, OpenOptions { parameters })` to pass a
`BTreeMap<String, String>` of server parameters. Native defaults include
`memory_budget=1G` and `log_disk_size=2G`. Ordinary parameters, including these
budgets, apply only when initializing a new data directory; reopening keeps
persisted values. Driver parameters `port` and `mysql_port_mode` apply on each
spawn.

`close(self)` consumes the handle, and `Drop` releases it if explicit close was
omitted. Closing does not delete data or guarantee immediate server-process
termination. Multiple handles can reuse one instance; keep a handle open until
its external SQL clients have disconnected.

## Package and deploy an application

Build and stage a complete application bundle with the Python 3.9+ helper:

```sh
python3 rust/scripts/package.py \
  --manifest-path rust/Cargo.toml \
  --example first_query \
  --output dist/seekdb-first-query

./dist/seekdb-first-query/first_query /tmp/seekdb-deployed-demo
```

For your own application, select its manifest and binary:

```sh
python3 /path/to/seekdb-bindings/rust/scripts/package.py \
  --manifest-path /path/to/my-app/Cargo.toml \
  --bin my-app \
  --output /path/to/dist/my-app
```

The helper builds with `--locked` and the `release` profile by default. Use
`--profile dev` for a development bundle or `--target <triple>` to select a
supported target. It reads the executable and native search paths from Cargo's
JSON output, so custom Cargo target directories work without path guessing.
The output directory must not already exist.

```text
my-app/
  my-app
  lib/
    libseekdb.so
    seekdb
    ...           # complete native dependencies, subdirectories and symlinks
```

Ship this entire directory. At runtime, the binding prefers `lib/` beside the
application executable. libseekdb discovers the seekdb server beside its own
shared library, and native dependencies use the bundle's relative loader paths.
The helper preserves that layout and executable permissions without changing
RPATHs. `cargo build` alone does not create this deployment bundle; a binary
copied out of Cargo's output must be accompanied by the staged `lib/` directory.

## Validation

```sh
cargo test --manifest-path rust/Cargo.toml --locked
python3 -m unittest discover -s rust/scripts -p 'test_*.py'
```

Tests cover lifecycle ownership, SQL writes and reads across shared lifecycle
handles, both runnable query examples, and the package helper. CI
also runs the packaged application with Cargo's build directory hidden and
its loader search environment cleared.
