---
name: build-seekdb-python-wheel
description: Build and verify pylibseekdb Python wheels from seekdb source on remote Linux x86_64 or macOS arm64 hosts. Use when a user asks to package seekdb from a commit, tag, or branch; the workflow fetches source through GitHub SSH, runs the seekdb-bindings build, applies release-only compiler policy, handles known remote failures, verifies both supported Python ABI wheels, and returns checksums and artifact paths.
---

# Build seekdb Python wheels

Use the scripts in `oceanbase/seekdb-bindings`. Keep every build in a new isolated directory on the requested remote host.

## Collect required inputs

Require all three inputs before changing the remote host:

- `build_host`: SSH target, such as a host alias, IP, or `user@host`.
- `seekdb_ref`: seekdb commit hash, tag, or branch.
- `wheel_version`: requested PEP 440 Python package version.

If any are absent, ask the user for the missing values. Do not invent, infer, or reuse defaults for them.

Determine the target platform from the request and remote host. Supported targets are Linux x86_64 and macOS arm64; ask if the requested target remains ambiguous after host inspection.

Also determine whether this is an official release package. If the request is ambiguous and this changes the build result, ask. Add `-DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN` only for an official release or when the user explicitly requests it. Do not add it to ordinary development packages.

## Prepare the remote build

1. Verify non-interactive SSH access, platform and architecture, free disk, and Python 3.11 or newer. Linux also requires a usable Docker or Podman runtime; macOS requires Xcode command-line tools and a usable macOS SDK.
2. Verify GitHub SSH access with `ssh -T git@github.com`; its expected authentication success message still returns a nonzero status.
3. Create a new build directory instead of reusing, deleting, or modifying an existing checkout.
4. Clone `git@github.com:oceanbase/seekdb-bindings.git` through SSH. Record its resolved commit. Initialize only its top-level submodules:

   ```bash
   git -C bindings -c url.git@github.com:.insteadOf=https://github.com/ \
     submodule update --init
   ```

   Do not use recursive submodule initialization: OpenSSL contains large optional test and fuzzing submodules that this wheel does not need.
5. Fetch seekdb through SSH into a separate checkout and detach at the resolved commit:

   ```bash
   git -C seekdb-src init
   git -C seekdb-src remote add origin git@github.com:oceanbase/seekdb.git
   git -C seekdb-src fetch --depth 1 origin "$seekdb_ref"
   git -C seekdb-src checkout --detach FETCH_HEAD
   git -C seekdb-src rev-parse HEAD
   ```

6. Create a dedicated host virtual environment. On pyenv hosts, resolve a real interpreter path such as `$(pyenv prefix 3.12)/bin/python`; do not trust a shim for a version that is not active.

If a network operation fails with `ProxyError`, HTTP 503 after CONNECT, or an apparent proxy tunnel failure, confirm direct connectivity and retry with every common proxy variable removed:

```bash
env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
    -u all_proxy -u ALL_PROXY command ...
```

Use the repository's configured package mirror when appropriate. Do not copy a differently versioned cached download under an expected filename; retain checksum integrity.

## Build on Linux x86_64

When the host checkout is mounted as `/seekdb`, current manylinux Git can reject it as dubious ownership. Check the isolated bindings script for an existing fix. If absent, add this line immediately after the inner container's `set -euo pipefail` in `scripts/build-seekdb-glibc228.sh`:

```bash
git config --global --add safe.directory /seekdb
```

Limit this compatibility edit to the isolated build checkout.

Run:

```bash
./scripts/build-pylibseekdb-wheel.sh \
  --build-seekdb \
  --seekdb-git-url git@github.com:oceanbase/seekdb.git \
  --seekdb-git-ref "$seekdb_ref" \
  --seekdb-repo "$seekdb_checkout" \
  --wheel-version "$wheel_version" \
  --platform linux \
  --arch x86_64
```

For an official release, append:

```bash
--seekdb-cmake-arg -DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN
```

Set `PYTHON` to the dedicated virtualenv interpreter. Preserve the full build log and monitor the remote process through completion. Expect `cp311-cp311` and `cp312-abi3` wheels tagged `manylinux_2_28_x86_64`.

## Build on macOS arm64

Use low parallelism because the native seekdb build is memory-intensive:

```bash
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
export CMAKE_BUILD_PARALLEL_LEVEL=2
export MAKEFLAGS=-j2
export TMPDIR=/tmp
```

`TMPDIR=/tmp` is required for tests that create a seekdb Unix socket. The default macOS per-user temporary path can make the socket path exceed the `AF_UNIX` limit and leave the test waiting even though seekdb started successfully.

The current bindings script derives native job counts from the host CPU count. In the isolated checkout, cap both of these build commands at `-j2` before starting:

- seekdb's `./build.sh ... --make -j...`
- libseekdb's `cmake --build ... -j...`

Run:

```bash
./scripts/build-pylibseekdb-wheel.sh \
  --build-seekdb \
  --seekdb-git-url git@github.com:oceanbase/seekdb.git \
  --seekdb-git-ref "$seekdb_ref" \
  --wheel-version "$wheel_version" \
  --platform macos \
  --arch arm64 \
  --seekdb-cmake-arg -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  --cmake-arg -DCMAKE_OSX_SYSROOT="$SDKROOT"
```

For an official release, append:

```bash
--seekdb-cmake-arg -DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN
```

Preserve the full build log and monitor free disk throughout the native build and wheel repair. Expect `cp311-cp311` and `cp312-abi3` wheels tagged `macosx_*_arm64`.

### Diagnose the CMake 4.2 protoc failure

If protobuf generation reports `Subprocess killed`, first run the exact vendor `protoc` command directly. If direct `protoc` and `/usr/bin/env ... protoc` succeed but the same command through `cmake -E env` fails, prefer a compatible CMake version. If that is unavailable, patch only the generated `deps/oblib/src/grpc/CMakeFiles/oblib_grpc.dir/build.make` in the isolated seekdb build so the failing command uses `/usr/bin/env` instead of `cmake -E env`, then retry the seekdb target incrementally at `-j1`. Record this workaround in the result. Do not apply it without reproducing this exact distinction.

## Verify before delivery

Do not report success merely because wheel files exist. Verify all applicable items:

1. `git rev-parse HEAD` in seekdb matches the commit resolved from `seekdb_ref`, and record the bindings commit.
2. For an official release, `build_release/CMakeCache.txt` contains `DEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN`; where possible, confirm the macro in an actual compiler command.
3. Confirm the staged seekdb is stripped and retain the debug-symbol path. On Linux, its maximum referenced GLIBC version must be no greater than `GLIBC_2.28`.
4. Confirm `python/pyproject.toml` returned to its original version after the temporary override.
5. Confirm wheel filenames and `METADATA` contain `wheel_version`.
6. Confirm cibuildwheel's built-in test passed for both wheels and the `cp312-abi3` wheel passed strict ABI3 auditing.
7. Run `scripts/verify-wheel.sh` explicitly against both wheels with real Python 3.11 and 3.12 interpreter paths. On macOS, keep `TMPDIR=/tmp` and restrict `PATH` per invocation when necessary so the script actually selects the intended interpreter.
8. On macOS, inspect repaired native dependencies with `delocate-listdeps` and `otool -L`; wheel binaries must not retain absolute Homebrew paths.
9. Compute SHA-256 for each wheel and copy the wheels to the requested local destination. If none was provided, use the current bindings checkout's ignored `build/wheelhouse/`.

Report the platform and architecture, resolved seekdb and bindings commits, release-flag policy, SDK and concurrency on macOS, any workaround used, wheel names, sizes, checksums, verification results, local paths, remote build directory, build log, and debug-symbol path.
