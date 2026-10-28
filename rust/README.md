# seekdb Rust binding 1.4.0

This crate starts or reuses a local seekdb instance, returns its MySQL-protocol
endpoint, and closes the lifecycle handle. Applications choose their own SQL
client. The examples use `mysql` and `mysql_async`; neither is a runtime
dependency of the binding.

## Build and run your first query

Run from this repository on a platform listed below:

```sh
cargo run --manifest-path rust/Cargo.toml --example first_query -- /tmp/seekdb-rust-demo
```

The example opens the database and connects using the returned Unix socket
(Linux/macOS) or TCP host and random port (Windows), as `root` with an empty
password to database `test`. It runs `SELECT 1` and prints `SELECT 1 = 1`.
It closes the SQL connection before releasing the lifecycle handle. The complete
source is in [`examples/first_query.rs`](examples/first_query.rs).

On Windows PowerShell, use a local data path, for example:

```powershell
cargo run --manifest-path rust/Cargo.toml --example first_query -- .\seekdb-rust-demo
```

The application's own Rust/MSVC runtime dependencies still apply on Windows;
embedding the native bundle does not change its compiler's CRT linkage.

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

On the first build, `build.rs` downloads the pinned seekdb 1.4.0.0 revision 2
native archive from OSS, verifies its size and SHA-256, and prepares the native cache.
The release version and manifest SHA-256 are pinned in
`Cargo.toml` under `[package.metadata.seekdb]`. `build.rs` obtains the matching
`manifest-<native-version>.json` from the configured download directory on a
metadata cache miss and verifies its exact bytes against that pin. It reuses
verified copies in `OUT_DIR` or the shared manifest cache before making a request.
The exact [release snapshot](bundles/manifest-1.4.0.0-r2.json) is also distributed
with the crate as a fallback when the request fails. A remote response with an
incorrect digest, schema or release identity fails the build; it is not silently
replaced with the snapshot. `bundle_manifest.rs` maps Cargo `TARGET` (including
ABI) to the verified manifest; it never selects `HOST` or fetches a latest
manifest. The crate version remains **1.4.0**; the independent native bundle
version is **1.4.0.0-r2**. Debug-symbol archives are not embedded.
The binding embeds the compressed archive with `include_bytes!`. Downloads
honor standard proxy variables and the platform TLS trust store (including
configured enterprise roots), and use Rust HTTP libraries; no external `curl`
or `tar` command is required.

Builds and running applications use one cache entry:

```text
<cache-root>/<target>/<native-version>/<full-archive-sha256>/
  bundle.tar.gz       # verified compressed bundle for later builds
  .complete           # native file completeness manifest
  ...                 # complete extracted bundle, including lib/ and licenses
```

The default root is `<user-cache>/seekdb/native`: on Linux `<user-cache>` is an
absolute `XDG_CACHE_HOME` or `~/.cache`; on macOS it is `~/Library/Caches`.
Windows uses `%LOCALAPPDATA%\seekdb\cache\native` directly as the root.
`SEEKDB_CACHE_DIR` selects a custom root for both the build and runtime.
Builds reuse the verified archive across projects, profiles, and Cargo target
directories, including after `cargo clean`. They also extract the native files
using the same private staging, atomic rename, and completion manifest as
runtime initialization. Building does not load libseekdb or start its server.

On the same machine, user, root, and target/version/hash, runtime initialization
reuses these files without extracting again. On another machine, in another
cache root, or after cache cleanup, it restores the entry from the embedded
archive without a download. libseekdb loads by its absolute path and starts
the bundled seekdb server. This does not depend on Cargo's build directory,
loader environment variables, or a `lib/` directory beside the application.
Unix directories, symlinks, and executable permissions are preserved. Windows
retains the complete payload with the layout normalization described below.

A valid existing `OUT_DIR` archive can seed or repair the shared cache offline.
If that cache cannot be prepared, the build warns and uses `OUT_DIR`. Each build
keeps an independently verified archive snapshot in `OUT_DIR` as its compiler
input, so clearing the user cache cannot remove a file needed by `include_bytes!`.
Build-time extraction needs writable storage; only runtime use needs permission
to load native code and execute the server. An explicit Rust API override is
runtime-only and may choose a different root from the build's cache.

Cargo's standard `CARGO_TARGET_DIR`, `--target-dir`, profiles, and supported
`--target` configuration still apply. For example:

```sh
CARGO_TARGET_DIR=/tmp/seekdb-target cargo test --manifest-path rust/Cargo.toml --locked
```

## Build-time download configuration and upgrades

The independent native release is selected by source-controlled Cargo metadata:

```toml
[package.metadata.seekdb]
native-version = "1.4.0.0-r2"
manifest-sha256 = "e423d7820050fd5918b1c07748b068d4fc6f3746af61767980a9d8bc1d4f9b49"
```

The default download directory is
`https://obbusiness-private.oss-cn-shanghai.aliyuncs.com/download-center/opensource/libseekdb/`.
To use an HTTPS mirror at build time:

```sh
SEEKDB_DOWNLOAD_BASE_URL=https://mirror.example/libseekdb/ cargo build --manifest-path rust/Cargo.toml
```

On PowerShell:

```powershell
$env:SEEKDB_DOWNLOAD_BASE_URL = 'https://mirror.example/libseekdb/'
cargo build --manifest-path rust/Cargo.toml
```

The override applies to both `manifest-<native-version>.json` and the runtime
archive filenames selected from it. The mirror must serve the exact pinned
manifest and archives; the override does not change versions, expected hashes,
or cache identities. A missing trailing slash is added. Only absolute HTTPS
directory URLs without credentials, query parameters or fragments are accepted.
Standard proxy settings and platform TLS trust still apply. Cargo reruns the
build script when the override changes; verified cached contents can still be
reused without contacting the new mirror. The override is read by `build.rs`;
changing it when launching an already-built application has no effect.

Manifest metadata is shared across targets in a separate cache under the same
root selected by `SEEKDB_CACHE_DIR` (or the platform default):

```text
<cache-root>/manifests/<native-version>/<full-manifest-sha256>/manifest.json
```

Each build also keeps its own verified manifest in `OUT_DIR`. Missing/corrupt
metadata is repaired through verified copies or a new request. Publishers stage
complete files and publish with atomic rename, without file locks. An unavailable
shared metadata cache warns and uses `OUT_DIR`, as the bundle cache does.
On Windows, metadata publication uses standard Rust rename handling for open
readers. If a foreign handle or filesystem policy still denies replacement,
the verified build-local copy remains usable and a later build can repair the
shared copy after the handle closes. Runtime
never reads this metadata cache or contacts OSS; its embedded archive and
`<root>/<target>/<native-version>/<full-archive-sha256>` entry are unchanged.
After preparing the cache with `SEEKDB_CACHE_DIR` explicitly set,
`python rust/tests/test_manifest_download.py` exercises a cold real manifest
request without a packaged copy, then Cargo builds using packaged fallback and
shared metadata with an unreachable download prefix. Linux CI runs these checks.

An offline build needs a verified bundle in `OUT_DIR` or the shared native cache;
the packaged manifest fallback supplies metadata, not the compressed runtime.

To upgrade a compatible native release:

1. Fetch the exact release manifest, verify its provenance, artifact hashes,
   architectures, ABI and minimum systems, and record its SHA-256.
2. Update only `native-version` and `manifest-sha256` in the metadata above.
   Add the exact JSON as `bundles/manifest-<native-version>.json` for offline
   fallback and crate source distribution.
3. Run platform tests, examples, deployments and `python rust/tests/check_package.py`,
   then update requirements and validation evidence. The package check derives
   the expected snapshot path from the same Cargo metadata.

Compatible releases need no Rust version literals changed in the downloader,
parser or file-watch logic. New target mappings, native ABI or layout changes
still require implementation review. The Rust crate version is maintained
separately. Once published, crates.io versions cannot be overwritten; shipping
updated sources/bundles there requires a new Rust crate release.

## Native platforms and requirements

| Cargo target | Native requirement | Runtime connection |
| --- | --- | --- |
| `x86_64-unknown-linux-gnu` | glibc 2.28+, x86_64 | Unix socket |
| `aarch64-unknown-linux-gnu` | glibc 2.28+, ARM64 | Unix socket |
| `x86_64-apple-darwin` | macOS 15.0+, Intel | Unix socket |
| `aarch64-apple-darwin` | macOS 15.0+, Apple Silicon | Unix socket |
| `x86_64-pc-windows-msvc` | x86_64 Windows, MSVC ABI; Windows Server 2022 CI baseline (preview) | TCP, returned host/port |

All five official runtime archives have been downloaded and their sizes and
SHA-256 values verified. ELF architecture/version requirements and Mach-O
architecture/deployment versions agree with the manifest. Windows has PE x86_64
binaries and bundled DLL dependencies. The manifest does **not** specify a
Windows minimum OS version; the PE subsystem version alone does not establish
one. Older Windows versions are not claimed supported. The CI matrix exercises
these five targets on native runners; adding a job is not a passing result.
See [validation evidence](docs/native-bundle-validation.md) for the distinction
between local runtime tests, cross-compilation, and pending CI results.

There are no assets for Linux musl, Windows ARM64/GNU, 32-bit systems, Android,
or iOS in this release manifest. Those targets fail with an explicit diagnostic.
Cross-compilation downloads and embeds the **target** archive without executing
it on the build host. Embedding does not remove OS, architecture, ABI, CPU, or
system-library requirements.

On Linux/macOS, load `libseekdb.so`/`libseekdb.dylib` directly through its C ABI;
the colocated service is `seekdb`. Windows loads the C driver `seekdb.dll` with
an explicit DLL-directory search policy. Its archive has `bin/seekdb.exe` and
`lib/*.dll`: extraction removes the long outer package-name directory, retains
all files, and copies the executable into `lib/` so its dependencies are
colocated. The binding configures that bundled executable through the existing
C API. It does not change the application's `PATH` or add an adapter library.

The r2 Windows C driver uses ANSI Windows APIs. Rust converts native paths to
the active Windows ANSI code page and rejects lossy conversions, UNC paths,
NULs, and paths of 240 UTF-16 units or more. Prefer short local paths; Unicode
paths require a code page that represents them (or the system UTF-8 code page).
Long-path support in Rust alone cannot fix the pinned C driver. Cache setup
uses Windows filesystem APIs; custom Windows cache roots must have a DACL that
excludes untrusted writers. On Unix, the binding checks ownership and group/other
write permissions.

## Runtime cache configuration

Cache root selection follows this precedence:

1. `seekdb::set_runtime_cache_dir(absolute_path)`.
2. The `SEEKDB_CACHE_DIR` environment variable, an absolute path.
3. The platform's default cache directory.

On Linux, the default root is
`$XDG_CACHE_HOME/seekdb/native` if `XDG_CACHE_HOME` is an absolute path, otherwise
`~/.cache/seekdb/native`. The macOS directory convention is
`~/Library/Caches/seekdb/native`. Windows uses the LocalAppData known folder,
with `seekdb\cache\native` appended.
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

For Windows PowerShell, an absolute short local-drive root can be selected with
`$env:SEEKDB_CACHE_DIR = 'C:\seekdb-cache'`; create a private directory writable
by your account. The default LocalAppData cache avoids requiring a drive-root
write permission. Use the same absolute local path convention for the Rust API.


A successful native initialization freezes the selected root for this copy of
the Rust dependency. Later API changes return an error. Libraries sharing the
same resolved seekdb dependency use this configuration; the final application
should configure it before any dependency opens a database. Failed setup can
be retried after correcting the directory. The cache setting selects where the
bundle is cached and extracted.

The cache must be private to the current user and permit native
library loading and server execution. Creating or repairing it requires writable
storage. Linux mounts with `noexec` produce
`Error::RuntimeCacheNoExec` containing the path and configuration guidance.
Filesystem failures preserve the path, operation, and underlying `io::Error` in
`Error::RuntimeCache`; other loader/security-policy errors retain their original
message rather than being labeled `noexec`.

Every `Seekdb::open` checks the selected entry, including required files,
symlinks, executable bits, and file sizes. A missing or incomplete cache is
restored from the embedded bytes, even if libseekdb is already loaded. A
directory rename publishes a complete entry. Concurrent publishers reuse a
winner when one is already available and discard their own staging directories.
They may independently download or extract the same bundle during a cold start;
there is no cache file lock.

On Linux/macOS, a damaged nonempty entry is repaired with atomic directory exchange
(`renameat2(RENAME_EXCHANGE)` on Linux; `renamex_np(RENAME_SWAP)` on macOS),
keeping the library path present throughout replacement. Filesystems that do
not support exchange return a cache error during such repairs; select another
cache filesystem or remove the damaged entry when no instance uses it. Archive
repairs use a sibling temporary file and atomic file rename.

Windows has no equivalent atomic nonempty-directory exchange. It prepares the
complete payload in a sibling staging directory, renames it into a new
`.generation-*/bundle/` within the same cache entry, then atomically renames the
completion inventory over `.complete`. Readers follow only a complete inventory;
previous generations remain for loaded DLLs and running servers. On another
open the existing C driver is pointed at the new bundled server. It never
deletes the current directory before publishing and uses no cache file lock.
Windows may refuse external cleanup while an executable or DLL is mapped;
close all consumers and allow their child processes to exit before cleanup.

Cache validation
checks completeness, not a cryptographic hash of every cached file on each
open. Keep the cache protected from untrusted writers.

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

Tests cover multi-process rename publication and repair, build/runtime cache
reuse, shared downloads, cache precedence,
concurrent extraction, layout preservation,
missing-file recovery with libseekdb loaded, Linux `noexec` diagnostics,
lifecycle ownership, and SQL access across shared handles. CI runs both SQL
examples and a downstream application installed by Cargo, then hides the build
directory and runs with loader variables cleared and fresh or deleted caches.

Run the application and dynamic-library deployment checks locally with
`python rust/tests/consumer/test_deployment.py` from the repository root on any
listed platform (Python 3.12+). The shell entry point remains available on Unix.
Linux CI adds
`--check-noexec` to test a mounted `noexec` filesystem; that option requires
`sudo`. See [the consumer test instructions](tests/consumer/README.md).


## Rust dynamic libraries and source distribution

A crate with `[lib] crate-type = ["cdylib"]` can depend on seekdb directly or
through another Rust library. When its exported functions use seekdb, the
compressed bundle is linked into the resulting `.so`, `.dylib`, or `.dll`.
Deploy that artifact and use an appropriate runtime cache on the destination;
it does not need a separate C/C++ adapter or copied native directory. The
`seekdb_consumer_ffi` library in `tests/consumer/` is only a deployment test
artifact. Independently built cdylibs can carry independent SDK state and
archives; configure each before its first native initialization.

`cargo package` includes the pinned manifest, build/cache sources, runtime
sources, examples, and documentation through an explicit include list.
`python rust/tests/check_package.py` inspects the actual `.crate` archive and
compiles its unpacked sources. Downloaded bundles and Cargo output are not
source-package inputs. No crates.io publish is performed. `publish = false`
remains intentional: maintainers still need to approve the platform validation
(including Windows preview limitations), native-binary/license redistribution,
crate ownership/release metadata, and enable publishing for a release. Native
packaging revisions alone do not require a Rust crate version bump.
