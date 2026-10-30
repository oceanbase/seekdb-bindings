# Rust consumer deployment tests

This workspace models an application depending on libraries B and C, both of
which depend on `seekdb`. It also builds a Rust `cdylib` that depends on B and
exports a C function for a Python `ctypes` probe. These are test fixtures with
`publish = false`; they are not additional SDK runtime libraries.

Run from the repository root on a platform listed in the Rust README, with Rust/Cargo
and Python 3.12+ installed:

```sh
python rust/tests/consumer/test_deployment.py
```

The script installs the application with Cargo, builds the dynamic library,
hides its build directory, and clears loader search-path overrides. It checks
actual SQL queries through installed and copied B/C applications, cache reuse,
missing-file recovery, deletion and concurrent cold/repair starts, then queries
SQL through the deployed dynamic library with fresh and deleted caches. Runtime
proxies point at an unreachable address to catch accidental network dependency.
The library is `.so` on Linux, `.dylib` on macOS, and `.dll` on Windows; this is
only a test artifact, not an added SDK dependency. Windows cleanup retries while
child processes release mapped files; cache deletion during execution is not
claimed to work there. Timeout or partial-spawn failures terminate and reap the
runner's own consumer processes before cleanup. Native servers are separate
children of those consumers; r2's asynchronous shutdown/readiness-timeout
behavior is described in the [native limitations](../../docs/native-bundle-validation.md).
The runner does not claim to kill every native descendant after arbitrary hangs.

To also check the error returned for a `noexec` cache filesystem, as CI does:

```sh
python rust/tests/consumer/test_deployment.py --check-noexec
```

This Linux-only option requires `sudo` permission to mount, change ownership of, and unmount
a temporary tmpfs. The default run does not require elevated privileges.
The script uses its own temporary build, install, deployment, and runtime-cache
directories, and removes them on exit. It does not move the caller's Cargo build
directory. Builds use the normal SDK bundle cache and may download the pinned
native archive if it is not already available.
