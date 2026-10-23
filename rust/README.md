# seekdb Rust binding (initial version)

This crate starts or reuses a local seekdb instance, returns its MySQL-protocol
endpoint, and closes the lifecycle handle. Use a separate MySQL client crate for
SQL. It wraps the public C lifecycle API in `../lib/include/seekdb/seekdb.h`.

## Build and test from this repository

Build on Linux x86_64:

```sh
cargo test --manifest-path rust/Cargo.toml
```

The build requires `curl` and `tar`. On the first Linux x86_64 build, `build.rs`
downloads the prebuilt `libseekdb` runtime bundle from OSS, verifies its pinned
SHA-256, and extracts it into Cargo's build output directory. The bundle
contains the C library, seekdb executable, and their native dependencies. To
build offline, or on another platform with an existing bundle, point
`SEEKDB_NATIVE_DIR` to its `lib/` directory:

```sh
SEEKDB_NATIVE_DIR=/path/to/lib cargo test --manifest-path rust/Cargo.toml
```

The bundle is pinned to seekdb 1.4.0.0 revision 1. Automatic download is
configured only for Linux x86_64. The macOS arm64 and Linux arm64 URL fields
are intentionally empty until their OSS objects are available; builds on those
platforms need `SEEKDB_NATIVE_DIR`. Windows is not supported by this Rust
binding yet. The Linux x86_64 bundle requires glibc 2.28 or newer.

## API

```rust
use seekdb::{OpenOptions, Seekdb};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let db = Seekdb::open("./data")?;
    let endpoint = db.connection_options()?;
    println!("{}: {}", endpoint.transport, endpoint.user);
    // Connect with a separate MySQL client using endpoint.host/port or
    // endpoint.unix_socket. Close that client before closing db.
    db.close()?;
    Ok(())
}
```

Use `Seekdb::open_with_options(path, OpenOptions { parameters })` to pass seekdb
server parameters. Ordinary parameters apply only during first initialization.
`Drop` closes a handle if `close` was not called. Closing does not delete data.

## Packaging boundary

This first version is marked `publish = false` while platform coverage and
application deployment are being validated. At runtime the binding looks for
the bundle's `lib/` directory in this order: `SEEKDB_NATIVE_DIR`, `lib/` beside
the application executable, then Cargo's build output. To deploy a built
application, ship the bundle's entire `lib/` directory beside the executable:

```text
my-app
lib/
  libseekdb.so  # libseekdb.dylib on macOS
  seekdb
  ...           # bundled native dependencies
```

`cargo build` downloads the bundle but does not copy it beside the final
application binary. A deployment step must stage it there, or set
`SEEKDB_NATIVE_DIR` to the deployed `lib/` directory.
