# Rust consumer deployment tests

This workspace models an application depending on libraries B and C, both of
which depend on `seekdb`. It also builds a Rust `cdylib` that depends on B and
exports a C function for a Python `ctypes` probe. These are test fixtures with
`publish = false`; they are not additional SDK runtime libraries.

Run from the repository root on Linux x86_64 with glibc 2.28 or newer, Rust/Cargo,
and Python 3 installed:

```sh
bash rust/tests/consumer/test-deployment.sh
```

The script installs the application with Cargo, builds the dynamic library,
hides its build directory, and clears loader search-path overrides. It checks
an actual SQL query through the installed B/C application with a fresh cache
and again after deleting that cache, then opens and closes a database through
the deployed dynamic library with its own fresh cache.

To also check the error returned for a `noexec` cache filesystem, as CI does:

```sh
bash rust/tests/consumer/test-deployment.sh --check-noexec
```

This option requires `sudo` permission to mount, change ownership of, and unmount
a temporary tmpfs. The default run does not require elevated privileges.
The script uses its own temporary build, install, deployment, and runtime-cache
directories, and removes them on exit. It does not move the caller's Cargo build
directory. Builds use the normal SDK bundle cache and may download the pinned
native archive if it is not already available.
