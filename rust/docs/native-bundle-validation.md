# Native bundle 1.4.0.0-r2 validation

The release manifest was retrieved on 2026-10-28 from:

<https://obbusiness-private.oss-cn-shanghai.aliyuncs.com/download-center/opensource/libseekdb/manifest-1.4.0.0-r2.json>

The managed environment's effective HTTP policy was inspected first: the OSS
host was allowed and enforcement was current. The checked-in JSON preserves the
release's original contents. Builds use it locally, never a remote latest file.
Rust crate version 1.4.0 is unchanged.

## Official artifacts

All five runtime archives were retrieved from the manifest's directory. Their
actual byte lengths and full SHA-256 digests match the pinned manifest. The
separate debug-symbol archives were not downloaded or embedded.

| Asset | Static inspection | Local runtime validation |
| --- | --- | --- |
| Linux x86_64 GNU | ELF x86_64; server requires glibc up to 2.28 | Passed on Linux x86_64 (glibc newer than 2.28) |
| Linux ARM64 GNU | ELF AArch64; server requires glibc up to 2.28 | Not run on this x86_64 host |
| macOS Intel | Mach-O x86_64; library and server minimum 15.0.0 | Not run on this Linux host |
| macOS ARM64 | Mach-O arm64; library and server minimum 15.0.0 | Not run on this Linux host |
| Windows x86_64 MSVC | PE x86_64; C driver DLLs, server EXE and dependency DLLs | Not run on this Linux host |

The Linux x86_64 tests use the **official r2 archive** and its real server,
not a locally rebuilt C library or replacement archive. They cover lifecycle,
SQL persistence/shared handles, reuse of the build-prepared runtime, missing
files with a library already loaded, complete cache deletion, synchronous and
Tokio SQL examples, installed/copied B/C consumers, and SQL through a Rust
cdylib. Deployment runs hide the build tree, clear loader overrides, and use
unreachable runtime HTTP proxies. Independent consumers share cold and damaged
caches. Linux noexec diagnostics are tested when `/dev/shm` is mounted noexec;
the local case was skipped because this host's `/dev/shm` permits execution.
CI mounts an explicit noexec tmpfs so the runtime diagnostic is actually exercised.

The small archives constructed inside cache unit tests are **fixtures**, not
release assets or evidence that another platform's server ran. They check
checksums, layout, permissions/symlinks on Unix, Windows layout normalization,
cache precedence, fallback, independent-process publication, and concurrent
repair without a missing published-library path. Windows native tests also
exercise generation-based repair; external deletion of mapped DLLs is excluded
because Windows denies it.

Cross-target `cargo check`/Clippy on Linux establishes compilation only. Native
GitHub Actions jobs are configured for Ubuntu x86_64/ARM64, macOS 15 Intel/ARM64,
and Windows Server 2022. Their status must be read from the PR's checks; the
presence of these jobs is not a test-pass claim. The manifest itself explicitly
records that its publisher did not run Linux ARM64 smoke tests.

## Remaining limits and publishing

The Linux tests did not run on a glibc 2.28 machine; that lower bound comes from
the manifest and ELF version requirements. The Windows manifest gives no
minimum OS version. Windows Server 2022 is the CI baseline; older versions are
not established by this work. Windows remains preview pending native CI and
maintainer review of its ANSI/local-path limits and generation retention.
Windows cache permissions inherit the containing directory's DACL; use a private
user directory. Long paths, UNC paths, or characters outside the active ANSI
code page are rejected instead of silently changing paths.

Neither runtime embedding nor static binary inspection proves compatibility
with all host CPU features or security policies. Linux/macOS damaged-directory
repair requires filesystem atomic exchange support. Old directories/generations
remain until operators clean them while no consumer is using them. Complete
cache deletion during arbitrary native execution is not guaranteed to work.

The package check inspects a real `cargo package` archive and compiles its
unpacked sources, including the pinned manifest and all build inputs. The crate
still has `publish = false`; no crates.io publish is performed. Release owners
must approve platform evidence, Windows preview status, redistribution/licenses,
crate ownership and metadata before enabling publication.
