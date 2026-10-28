# Native bundle 1.4.0.0-r2 validation

The release manifest was retrieved on 2026-10-28 from:

<https://obbusiness-private.oss-cn-shanghai.aliyuncs.com/download-center/opensource/libseekdb/manifest-1.4.0.0-r2.json>

The managed environment's effective HTTP policy was inspected first: the OSS
host was allowed and enforcement was current. The checked-in JSON preserves the
release's original contents. Its exact SHA-256 is pinned alongside the native
version in `[package.metadata.seekdb]`. Builds request only that version on a
metadata cache miss, verify its digest and identity, and can use the packaged
snapshot on a transport failure; no remote latest file is used.
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

## Earlier native CI evidence

The implementation commit `07f5f13d8c0d931dcc87101068275ea224ac3460` passed
all five jobs in the [native workflow run](https://github.com/hnwyllmm/seekdb-bindings/actions/runs/40651204407).
These are actual native runs with the official pinned runtime bundle:

| Target | Native runner | Full workflow result |
| --- | --- | --- |
| Linux x86_64 GNU | Ubuntu 24.04 x86_64 | Passed |
| Linux ARM64 GNU | Ubuntu 24.04 ARM64 | Passed |
| macOS Intel | macOS 15 Intel | Passed |
| macOS ARM64 | macOS 15 Apple Silicon | Passed |
| Windows x86_64 MSVC | Windows Server 2022 | Passed |

Each job passed lifecycle/SQL/cache tests, formatting and Clippy, synchronous
and Tokio SQL examples, installed/copied applications, SQL through the deployed
Rust cdylib, concurrent cold/repair consumers, and a real source-package check.
Both Linux jobs additionally mounted and tested an explicit noexec tmpfs. The
Windows tests exercised retained-generation repair while the driver was loaded
and checked that prior generations remain readable. Whole-cache deletion was
tested between consumers on Windows, rather than unlinking mapped executables.

Local cross-target checks establish compilation only; the native CI results
above provide separate execution evidence. The manifest publisher did not run
Linux ARM64 smoke tests, but this implementation has now passed native ARM64 CI.
The evidence above refers to the original platform implementation commit.
The subsequent configurable manifest-download change has separate cache/pinning
and mirror tests; its latest native validation status is available in the
[PR checks](https://github.com/oceanbase/seekdb-bindings/pull/84/checks).

## Manifest-download validation

The manifest was fetched again from OSS and its SHA-256 remained
`e423d7820050fd5918b1c07748b068d4fc6f3746af61767980a9d8bc1d4f9b49`.
Tests cover exact target selection, mirror directory normalization for both
manifest and archive URLs, rejection of unsafe URL forms, a synthetic future
release driven only by Cargo metadata, transport fallback, fatal digest/identity
mismatches, corrupt/missing metadata caches, unavailable user storage, and
concurrent atomic file publication. The synthetic future release is a test
fixture, not an existing native bundle or runtime compatibility claim.
`tests/test_manifest_download.py` also exercises real Cargo builds: a cold
remote request with the packaged snapshot removed, a request failure using the
packaged fallback, and shared metadata reuse with an unreachable URL and no
packaged snapshot. Its isolated native cache is seeded with the official archive.

## Remaining limits and publishing

The Linux tests did not run on a glibc 2.28 machine; that lower bound comes from
the manifest and ELF version requirements. The Windows manifest gives no
minimum OS version. Windows Server 2022 is the CI baseline; older versions are
not established by this work. Windows remains preview while maintainers review native CI evidence, its
unspecified minimum OS, ANSI/local-path limits, and generation retention.
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
