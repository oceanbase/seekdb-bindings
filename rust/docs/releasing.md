# Publishing the seekdb Rust crate

The crates.io package and Rust import are both `seekdb`. This release prepares
crate **1.4.0** with native bundle **1.4.0.0-r2**. The native revision and manifest
digest live in `[package.metadata.seekdb]`; changing them does not require a crate
version change before its first publication. After publication, crates.io versions
are immutable, so any later source or embedded-bundle change needs a new crate
version.

`publish = ["crates-io"]` enables the intended registry. Merging this configuration
does not publish anything. There is no workflow that uploads the crate and no
registry credential is required for the checks below.

## Registry name and ownership

On 2026-10-30, the public crates.io API returned 404 (`crate does not exist`) for
`seekdb`, its owners endpoint, and `seekdb-rust`. This is an observation, not a
name reservation or a guarantee that registration will succeed. There is no
existing public `seekdb` owner or `1.4.0` version to verify from those responses.

Before the first upload, the release maintainer must check the current
[crate record](https://crates.io/api/v1/crates/seekdb) again and use the project's
intended crates.io publishing account. If the name has since been registered,
confirm its [owners](https://crates.io/api/v1/crates/seekdb/owners) and ownership
access; do not silently publish under another name or overwrite the planned
version. Arrange the project's maintainer/team ownership after a successful first
publication. A dry-run does not authenticate or prove permission to upload.

## Release scope

The release includes GNU/Linux x86_64 and ARM64 (glibc 2.28+), macOS Intel and
Apple Silicon (15.0+), and Windows x86_64 MSVC as **preview**. The five native
jobs on the final implementation commit `a803099d9a36be82abb8737704806c1e4fe99ea4`
[passed](https://github.com/hnwyllmm/seekdb-bindings/actions/runs/40683968145).
Run the workflow again on the publication commit; historical CI is supporting
evidence, not a substitute for that run. The glibc 2.28 floor is based on manifest
and binary inspection rather than execution on a glibc 2.28 host. Windows Server
2022 is the tested baseline; r2 does not declare a minimum Windows version.
Windows also retains the documented ANSI/local-path and mapped-file restrictions.

The published r2 driver does not synchronously terminate a spawned native server
when readiness times out. The server may outlive the failed open, and normal
last-client shutdown is asynchronous. The test runner only guarantees cleanup of
its own consumer processes. This release retains that documented behavior;
resolving the review thread did not fix it. A fix needs a coordinated native
startup/ownership policy, concurrent-client and timeout regression tests on Unix
and Windows, and a newly published verified bundle. Do not describe this release
as guaranteeing server exit before cache cleanup after a failed open.

Read [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) before distribution.
The crate includes its Apache-2.0 license and a verified copy of the engine's
NOTICE, but the bundle's notice inventory does not cover every linked/shipped
dependency. Completing that native redistribution inventory, supplying required
notices and any applicable source/relink materials remains a publication
prerequisite for the release maintainer/native package producer. This PR does not
claim legal clearance for the prebuilt binaries.

## Check the final source package

From a clean checkout of the reviewed publication commit, with current stable
Rust and Python 3.12+ installed:

```sh
cargo test --manifest-path rust/Cargo.toml --locked
cargo fmt --manifest-path rust/Cargo.toml --all -- --check
cargo clippy --manifest-path rust/Cargo.toml --all-targets --locked -- -D warnings
python rust/tests/run_examples.py
python rust/tests/consumer/test_deployment.py
python rust/tests/check_package.py
python rust/tests/check_docs.py
```

CI runs these on all five native targets, with an explicit noexec mount and
manifest-download/fallback checks on both Linux targets. The package check runs
`cargo publish --dry-run --locked --registry crates-io` with verification enabled,
checks the archive's metadata, manifest pin, build sources, examples and notices,
rejects native binary/archive build outputs, and builds the unpacked crate.
It never uploads. For local edits only, pass `--allow-dirty` to the package check;
omit that option for the release. Cargo controls `CARGO_TARGET_DIR` normally.

The crate source archive does not contain a native runtime. A user's first build
needs network access to the exact pinned manifest/archive or a prepared verified
cache. A built application/cdylib embeds the archive and restores it offline.
docs.rs receives only the source package and has no network; its `DOCS_RS` mode
verifies the packaged manifest and renders API docs without a runtime payload.
Leave `DOCS_RS` unset in application, deployment and publish checks.

## Upload and validate

Once registry access, current CI, distribution requirements and release scope
are confirmed, the release maintainer can run Cargo's normal publish command
from that same clean commit:

```sh
cargo publish --manifest-path rust/Cargo.toml --locked --registry crates-io
```

This is a separate, explicit release action. Keep credentials out of the
repository. After upload, confirm the registered repository/version/owners and
docs.rs build. Build a fresh consumer using `seekdb = "=1.4.0"` from crates.io
without path or Git patches, run the first SQL query, and repeat deployment
verification on the release platforms. Only then announce the published version.

Downstream libraries that publish to crates.io must use a registry dependency
version and wait until `seekdb` is published. End users use `cargo install` for an
application depending on seekdb; the seekdb crate itself supplies no installable
binary. There is no additional C/C++ adapter library to publish.
