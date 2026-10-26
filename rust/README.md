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

## Embedded native runtime

On the first build, `build.rs` downloads the pinned seekdb 1.4.0.0 revision 1
native archive from OSS and verifies its SHA-256. The binding embeds that
compressed archive with `include_bytes!`. Later builds reuse the verified
archive in Cargo's `OUT_DIR`. Downloads honor standard proxy variables and use
Rust HTTP libraries; no external `curl` or `tar` command is required.

At runtime, the binding extracts the embedded archive into a user cache, loads
libseekdb by its absolute path, and launches the bundled seekdb server. This
needs no runtime download and does not depend on Cargo's build directory,
loader environment variables, or a `lib/` directory beside the application.
The archive's directories, symlinks, and executable permissions are preserved.

Cargo's standard `CARGO_TARGET_DIR`, `--target-dir`, profiles, and supported
`--target` configuration still apply. For example:

```sh
CARGO_TARGET_DIR=/tmp/seekdb-target cargo test --manifest-path rust/Cargo.toml --locked
```

Only Linux x86_64 with glibc 2.28 or newer has an automatic bundle URL at
present. macOS arm64 and Linux arm64 need their bundle URLs configured before
they can build; Windows native bundles are not supported. Embedding does not
remove native OS, architecture, or glibc requirements.

## Runtime cache configuration

Cache root selection follows this precedence:

1. `seekdb::set_runtime_cache_dir(absolute_path)`.
2. The `SEEKDB_CACHE_DIR` environment variable, an absolute path.
3. The platform's default cache directory.

For the currently supported Linux target, the default root is
`$XDG_CACHE_HOME/seekdb/native` if `XDG_CACHE_HOME` is an absolute path, otherwise
`~/.cache/seekdb/native`. The macOS directory convention is
`~/Library/Caches/seekdb/native`; native platform support is still required.
The extracted entry is `<root>/<target>/<native-version>/<full-archive-sha256>`.

An application can configure the SDK before its first native initialization:

```rust
seekdb::set_runtime_cache_dir("/data/my-app/runtime-cache")?;
let database = seekdb::Seekdb::open("/data/my-app/database")?;
```

An operator can change the location without rebuilding the application:

```sh
SEEKDB_CACHE_DIR=/data/my-app/runtime-cache ./my-app /data/my-app/database
```

A successful native initialization freezes the selected root for this copy of
the Rust dependency. Later API changes return an error. Libraries sharing the
same resolved seekdb dependency use this configuration; the final application
should configure it before any dependency opens a database. Failed setup can
be retried after correcting the directory. The cache setting only selects the
extraction location; the removed `SEEKDB_NATIVE_DIR` and
`SEEKDB_BUILD_NATIVE_DIR` overrides remain unsupported.

The cache must be user-owned, writable, not writable by other users, and allow
native library loading and server execution. Linux mounts with `noexec` produce
`Error::RuntimeCacheNoExec` containing the path and configuration guidance.
Filesystem failures preserve the path, operation, and underlying `io::Error` in
`Error::RuntimeCache`; other loader/security-policy errors retain their original
message rather than being labeled `noexec`.

Every `Seekdb::open` checks the selected entry, including required files,
symlinks, executable bits, and file sizes. A missing or incomplete cache is
restored from the embedded bytes, even if libseekdb is already loaded. A
process-safe file lock and staging-directory rename coordinate concurrent
initialization. This is a completeness check, not a cryptographic check of every
cached file on each open. Keep the cache protected from untrusted writers.

Database data stays in the separate directory passed to `Seekdb::open`. On
Linux, unlinking already mapped native files generally leaves an active instance
running; later native file accesses or server starts can still need those files.
The binding restores them before another open but cannot guarantee recovery
from arbitrary deletion during native execution. Incomplete entries are retired
without overwriting mapped files and are not automatically deleted; old versions
and retired entries consume disk space until cleaned when no instance uses them.
The compiled artifact also retains the compressed archive, so embedding adds
binary size, extraction time, and disk usage. See the
[design and accepted risks](docs/embedded-native-runtime.md).

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

## Build and deploy an application

Build your application's binary with normal Cargo commands:

```sh
cargo build --release --locked --manifest-path /path/to/my-app/Cargo.toml
```

Copy the resulting executable to the destination machine and run it. Its
embedded bundle supplies the native runtime; no Python packaging helper or
separately staged `lib/` directory is needed. The destination must meet the
native platform requirements and have a usable runtime cache and data directory.
For this repository's runnable example:

```sh
cargo build --release --locked --manifest-path rust/Cargo.toml --example first_query
cp rust/target/release/examples/first_query /path/to/deploy/first_query
/path/to/deploy/first_query /path/to/database
```

The example path above assumes Cargo's default target directory. When using
`CARGO_TARGET_DIR` or `--target-dir`, copy from your chosen output directory.

A downstream Rust library can depend on seekdb normally and expose its own API;
its callers do not need to copy native files. An application's `cargo install`
also builds the embedded runtime into its installed executable. Within one
Cargo dependency graph, libraries B and C that resolve to the same seekdb crate
share that dependency. Different versions or separately built dynamic libraries
can carry separate embedded archives.

Publishing a library or application to crates.io publishes its source package,
not its compiled dependencies or Cargo `OUT_DIR` contents. The downstream build
obtains the pinned native archive and embeds it. The crate remains
`publish = false` until its release and native platform coverage are ready.

## Validation

```sh
cargo test --manifest-path rust/Cargo.toml --locked
cargo fmt --manifest-path rust/Cargo.toml --all -- --check
cargo clippy --manifest-path rust/Cargo.toml --all-targets --locked -- -D warnings
```

Tests cover cache precedence, concurrent extraction, layout preservation,
missing-file recovery with libseekdb loaded, Linux `noexec` diagnostics,
lifecycle ownership, and SQL access across shared handles. CI runs both SQL
examples and a downstream application installed by Cargo, then hides the build
directory and runs with loader variables cleared and fresh or deleted caches.
