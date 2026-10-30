# Publishing the seekdb Rust crate

The crates.io package and Rust import are both `seekdb`. This release prepares
crate **1.4.0** with native bundle **1.4.0.0-r2**. The native revision and manifest
digest live in `[package.metadata.seekdb]`; changing them does not require a crate
version change before its first publication. After publication, crates.io versions
are immutable, so any later source or embedded-bundle change needs a new crate
version.

`publish = ["crates-io"]` enables the intended registry. Merging changes does not
publish anything. The manual `publish-rust.yml` workflow uses Trusted Publishing
to upload a new version after its tests and package checks pass. The validation
workflow and the checks below never upload, and need no registry credential.

## Registry name and ownership

Check the current [crate record](https://crates.io/api/v1/crates/seekdb) and
[owners](https://crates.io/api/v1/crates/seekdb/owners) using the project's intended
crates.io account. A first publication requires an API token; Trusted Publishing
is configured on an already published crate. Existing versions cannot be
overwritten. A dry-run does not authenticate or prove permission to upload.

## Configure GitHub Actions publishing

An owner of the published **seekdb** crate must add a GitHub Trusted Publisher in
its crates.io Settings → Trusted Publishing page:

| Field | Value |
| --- | --- |
| Repository owner | `oceanbase` |
| Repository name | `seekdb-bindings` |
| Workflow filename | `publish-rust.yml` |
| Environment | Leave empty; this workflow does not select an environment |

The workflow filename is a basename, not `.github/workflows/publish-rust.yml`.
Each crate has its own configuration: a trusted publisher on
`seekdb-jemalloc-sys` does not authorize publishing `seekdb`. No long-lived
crates.io token or GitHub Secret is used. The publishing job requests an OIDC
identity with `id-token: write`, and the pinned official
[crates-io-auth-action](https://github.com/rust-lang/crates-io-auth-action)
exchanges it for a temporary publish token after preflight succeeds.

After a version-bump PR has passed the five-platform validation workflow and
merged, trigger publication from `main`:

```sh
gh workflow run publish-rust.yml --repo oceanbase/seekdb-bindings --ref main
gh run list --repo oceanbase/seekdb-bindings --workflow publish-rust.yml --limit 1
```

The GitHub CLI must be authenticated as a user allowed to dispatch workflows on
the upstream repository. The web equivalent is Actions → Publish seekdb → Run
workflow, selecting `main`. The workflow must first exist on the default branch.

Only a manual dispatch in `oceanbase/seekdb-bindings` on `main` runs the publish
job; pushes, PRs and runs on forks do not publish. Concurrent dispatches are
serialized without cancelling a running upload. The job runs Linux runtime tests
and the full package dry-run/checker, then authenticates and runs `cargo publish`.
It publishes the version already declared in `rust/Cargo.toml`; it does not bump
versions. Do not dispatch for an already published version. The crate version
remains independent of the pinned native bundle revision.

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
then runs `cargo package --no-verify` to generate the source archive it inspects.
It checks that archive's metadata, manifest pin, build sources, examples and notices,
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
