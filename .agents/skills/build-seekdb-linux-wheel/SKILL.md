---
name: build-seekdb-linux-wheel
description: Build and verify pylibseekdb Linux x86_64 manylinux wheels from seekdb source on a remote host. Use when a user asks to package a Linux Python wheel for seekdb from a commit, tag, or branch; the workflow obtains source through GitHub SSH, runs the seekdb-bindings manylinux build, handles known remote-host failures, verifies both supported Python ABI wheels, and returns checksums and artifact paths.
---

# Build seekdb Linux wheels

Use the scripts in `oceanbase/seekdb-bindings`; keep each build in a new isolated remote directory.

## Collect required inputs

Require all three inputs before changing the remote host:

- `build_host`: SSH target, such as a host alias, IP, or `user@host`.
- `seekdb_ref`: seekdb commit hash, tag, or branch.
- `wheel_version`: requested PEP 440 Python package version.

If any are absent, ask the user for the missing values. Do not invent, infer, or reuse defaults for them.

Also determine whether this is an official release package. If the request is ambiguous and this changes the build result, ask. Add `-DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN` only for an official release or when the user explicitly requests it. Do not add it to ordinary development packages.

## Prepare the remote build

1. Verify non-interactive SSH access, Linux x86_64, free disk, Python 3.11 or newer, and a usable Docker or Podman runtime.
2. Verify GitHub SSH access with `ssh -T git@github.com`; its expected success message still returns a nonzero status.
3. Create a new build directory instead of reusing or deleting an existing checkout.
4. Clone `git@github.com:oceanbase/seekdb-bindings.git` through SSH. Initialize only its top-level submodules:

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

6. Create a dedicated host virtual environment. On pyenv hosts, resolve an actual interpreter path such as `$(pyenv prefix 3.12)/bin/python`; do not trust `command -v python3.12` when it only returns a shim for a version that is not active.

When the host checkout is mounted as `/seekdb`, current manylinux Git can reject it as dubious ownership. Check the isolated bindings script for an existing fix. If absent, add this line immediately after the inner container's `set -euo pipefail` in `scripts/build-seekdb-glibc228.sh`:

```bash
git config --global --add safe.directory /seekdb
```

Limit this compatibility edit to the isolated remote build checkout.

## Build

Run `scripts/build-pylibseekdb-wheel.sh` from the remote bindings checkout with:

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

Set `PYTHON` to the dedicated virtualenv interpreter. Preserve the full build log and monitor the remote process through completion. The expected result is two wheels: `cp311-cp311` and `cp312-abi3`, both tagged `manylinux_2_28_x86_64`.

## Retry network failures

If Git, pip, curl, wget, or a GitHub release download fails with `ProxyError`, HTTP 503 after CONNECT, or an apparent proxy tunnel failure, retry the failing probe or build with all common proxy variables removed:

```bash
env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
    -u all_proxy -u ALL_PROXY command ...
```

Use the repository's configured Tsinghua PyPI mirror when appropriate. Confirm direct connectivity before restarting a long build. Do not copy a differently versioned cached `virtualenv.pyz` under a new name; retain checksum integrity.

## Verify before delivery

Do not report success merely because wheel files exist. Verify all of the following:

1. `git rev-parse HEAD` in seekdb matches the commit resolved from `seekdb_ref`.
2. For an official release, `build_release/CMakeCache.txt` contains `DEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN`; where possible, also confirm the macro in an actual compiler command.
3. The staged `seekdb` is stripped and its maximum referenced GLIBC version is no greater than `GLIBC_2.28`.
4. `python/pyproject.toml` returned to its original version after the temporary wheel-version override.
5. Wheel filenames and `METADATA` contain the requested version.
6. cibuildwheel's built-in tests passed for both wheels; the `cp312-abi3` wheel passed strict ABI3 auditing.
7. Run `scripts/verify-wheel.sh` explicitly against both output wheels using real Python 3.11 and 3.12 interpreter paths. This exercises the native hybrid-search path plus PyMySQL and aiomysql.
8. Compute SHA-256 for each wheel and copy the wheels to the requested local destination, defaulting to the current bindings checkout's ignored `build/wheelhouse/` only when the user gave no destination.

Report the resolved seekdb commit, bindings commit, release-flag policy, wheel names, sizes, checksums, verification result, local paths, remote build directory, build log, and debug-symbol path.
