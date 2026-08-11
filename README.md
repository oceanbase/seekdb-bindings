# seekdb-bindings

C library, CLI, and Python bindings for [seekdb](https://github.com/oceanbase/seekdb).

The core shared library (`libseekdb`, with a `libseekdb_driver` compatibility symlink) is implemented in **C11** (`lib/src/*.c`) and does not link against `libstdc++` or `libc++` (checked in CI via `readelf`); embedders only need a C toolchain to build and pthreads at runtime. The CLI (`seekdb_cli`) is also implemented in C11 (`lib/src/seekdb_cli.c`). The bundled **seekdb server binary** is a separate C++ product and may depend on `libstdc++`; issue #6 applies only to `libseekdb`. C++ is also used for gtest-based integration tests and the optional nanobind Python extension.

## Layout

```
seekdb-bindings/
├── lib/
│   ├── include/seekdb.h        public C API
│   ├── src/                    library + CLI sources
│   └── tests/                  gtest cases
├── python/                     nanobind module + cibuildwheel config
├── scripts/                    wheel build, seekdb manylinux build, verify-wheel, etc.
└── deps/                       vendored mariadb-connector-c, googletest
```

## Clone

The repo uses git submodules (mariadb-connector-c, googletest). Clone with `--recursive`:

```sh
git clone --recursive https://github.com/cao1629/seekdb-bindings.git
cd seekdb-bindings
```

If already cloned without `--recursive`:

```sh
git submodule update --init --recursive
```

## Build

All builds go to a single CMake build directory — `build/` at the repo root by convention, but any location works. The `SEEKDB_BIN` env var must point to a built `seekdb` binary — it's copied next to `libseekdb` so the CLI / Python bindings can use it.

After configuration (per-platform commands below), choose a `make` target from inside `build/`:

| target | output |
|---|---|
| `make seekdb` | `libseekdb` (shared; `libseekdb_driver` symlink for compatibility) |
| `make seekdb_cli` | `seekdb_cli` (interactive SQL client) |
| `make pylibseekdb` | `pylibseekdb.cpython-*.so` (Python extension) |
| `make seekdb_tests` | `test_one_client_process`, `test_two_clients_threads` |
| `make wheel` | `wheelhouse/pylibseekdb-*.whl` via cibuildwheel (runs Python integration tests per wheel) |
| `make verify-wheel` | Re-run Python integration tests against wheels in `build/wheelhouse/` |
| `make` | everything above |

`build/seekdb` (a copy of `$SEEKDB_BIN`) ships alongside the library.

### Linux

```sh
export SEEKDB_BIN=/path/to/seekdb
cmake -S . -B build
cd build
make seekdb
```

### Mac

Identical to Linux, plus `-DWITH_EXTERNAL_ZLIB=YES` (the vendored zlib in mariadb-connector-c doesn't build cleanly on macOS):

```sh
export SEEKDB_BIN=/path/to/seekdb
cmake -S . -B build -DWITH_EXTERNAL_ZLIB=YES
cd build
make seekdb
```

### Windows (PowerShell)

The build commands above use POSIX `export` syntax. On Windows PowerShell, the equivalents are:

```powershell
# environment variables — set per session, not persisted
$env:SEEKDB_BIN = "C:\path\to\seekdb.exe"

# prepend ninja + LLVM/clang-cl to PATH (per session)
$env:PATH = "C:\path\to\ninja;C:\path\to\llvm18\bin;" + $env:PATH

# load MSVC's INCLUDE / LIB / linker into the current shell
# (one-shot per PowerShell session — needed by both cl.exe and clang-cl)
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64
```

Then the normal CMake + Ninja flow:

```powershell
cd <repo>
cmake -S . -B build -G Ninja `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cd build
ninja seekdb
```

Persist env vars across PowerShell sessions either via your `$PROFILE` file or with `[Environment]::SetEnvironmentVariable("SEEKDB_BIN", "C:\path\to\seekdb.exe", "User")`. The dev-shell load must be re-run every session — there's no good way to bake it in.

If running `.ps1` scripts is blocked by execution policy, allow them for the current process only:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
```

## CMake options

| option | default | what it does |
|---|---|---|
| `SEEKDB_DRIVER_ENABLE_LOG` | OFF | enables `tlog()` debug prints in libseekdb |
| `SEEKDB_BIN` | `$SEEKDB_BIN` env | path to the seekdb binary |
| `SEEKDB_BUILD_PYTHON` | ON | configure the Python extension; set OFF for a C-only build/package |

Example:

```sh
cmake -S . -B build -DSEEKDB_DRIVER_ENABLE_LOG=ON
cmake --build build
```

## Build C SDK release packages

`scripts/build-libseekdb-package.sh` creates a native C SDK archive from two
independently selected source revisions:

- an `oceanbase/seekdb` branch, tag, or commit for the embedded server;
- a `seekdb-bindings` branch, tag, or commit for `libseekdb` and `seekdb.h`.

Run the same script on one native builder for each release platform. Linux uses
the manylinux 2.28 container for an explicit glibc 2.28 baseline. The macOS
archive is built natively and the requested macOS major/minor version is
validated before compiling.

For repeated Linux releases, build the provided builder image once to avoid
installing the EL8 compiler prerequisites on every attempt:

```sh
docker build -t libseekdb-manylinux2_28:local \
  -f scripts/docker/libseekdb-manylinux.Dockerfile scripts/docker
```

```sh
# Linux x86_64 builder
./scripts/build-libseekdb-package.sh \
  --version 1.3.0 --revision 1 \
  --seekdb-ref release/1.3.0 \
  --bindings-ref main \
  --platform linux --arch x86_64 \
  --linux-image libseekdb-manylinux2_28:local

# Linux aarch64 builder
./scripts/build-libseekdb-package.sh \
  --version 1.3.0 --revision 1 \
  --seekdb-ref release/1.3.0 \
  --bindings-ref main \
  --platform linux --arch aarch64 \
  --linux-image libseekdb-manylinux2_28:local

# Apple Silicon builder running macOS 15.6
./scripts/build-libseekdb-package.sh \
  --version 1.3.0 --revision 1 \
  --seekdb-ref release/1.3.0 \
  --bindings-ref main \
  --platform macos --arch arm64 --macos-version 15.6
```

Use the product version for API/server releases and increment `--revision`
when rebuilding the same source release for a packaging-only correction. The
two resolved source commit IDs are also part of each archive name, so a branch
moving between builds cannot silently produce the same artifact identity.
Examples:

```text
libseekdb-1.3.0-r1-linux-x86_64-glibc2.28-sdb0123456789ab-bndabcdef012345.tar.gz
libseekdb-1.3.0-r1-linux-aarch64-glibc2.28-sdb0123456789ab-bndabcdef012345.tar.gz
libseekdb-1.3.0-r1-macos15.6-arm64-sdb0123456789ab-bndabcdef012345.tar.gz
```

Every archive has a sibling `.sha256` file. It contains this layout:

```text
include/seekdb.h
lib/libseekdb.so             # libseekdb.dylib on macOS
lib/libseekdb_driver.so      # compatibility symlink; .dylib on macOS
lib/seekdb                   # must remain next to libseekdb for auto-discovery
lib/<runtime dependencies>
bin/seekdb                   # convenience symlink to ../lib/seekdb
BUILD-INFO.txt               # refs, full commits, platform, ABI
DEPENDENCIES.txt             # bundled library provenance
README.txt                   # package contents and usage notes
seekdb-version.txt           # output of `seekdb -V` during packaging
SHA256SUMS                   # checksum of every regular file in the archive
licenses/

Linux glibc libraries and the ELF loader are intentionally not bundled; the
`glibc2.28` archive tag declares that host ABI requirement. Other non-system
dynamic dependencies of both `seekdb` and `libseekdb` are collected
recursively and use package-relative RPATHs. On macOS, non-system dylibs are
collected recursively, rewritten to package-relative install names, and
ad-hoc signed after rewriting.

The Linux builder installs the EL8 system Rust/Cargo toolchain by default. Both
Linux and macOS use USTC's Cargo sparse registry mirror while compiling seekdb;
an explicitly requested rustup toolchain also uses USTC's Rust distribution
mirror. Pass `--rust-mirror none` to use the upstream services, or
`--rust-toolchain <version>` to pin a non-system Rust release. The actual
`rustc --version`, selected mirror, and requested toolchain are recorded in
`BUILD-INFO.txt`.

The script also keeps seekdb's downloaded dependency RPMs in
`build/libseekdb-dep-cache/<platform>-<arch>` by default. Use
`--dep-cache-dir` to place that cache on a CI volume.

Verify an artifact before publishing or installing it:

```sh
sha256sum -c libseekdb-*.tar.gz.sha256       # Linux
shasum -a 256 -c libseekdb-*.tar.gz.sha256  # macOS
```

## Format source code

Install `clang-format-14` (or `clang-format` 14.x; see `.clang-format-version`),
configure the project as usual, then run:

```sh
cmake --build build --target format
```

The `format` target rewrites the repository's C, C++, and nanobind source files using the
root `.clang-format` style.

## Use seekdb_cli

Interactive SQL shell, same shape as `mysql` / `mariadb`:

```sh
build/seekdb_cli [db_dir]
```

`db_dir` defaults to `./seekdb.db` (created if missing). The seekdb binary is auto-discovered next to `libseekdb`.

## Prepare Python environment

`make pylibseekdb` and `make wheel` need a Python toolchain. Use a venv to keep `nanobind` (required by `make pylibseekdb` at configure time) and `cibuildwheel` (required by `make wheel`) out of the system Python.

### Linux / Mac

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install nanobind cibuildwheel
```

### Windows (PowerShell)

```powershell
py -3 -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install nanobind cibuildwheel
```

If `Activate.ps1` is blocked by execution policy, allow it for this process only:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
```

`cibuildwheel` additionally needs Docker running on Linux (it builds inside the manylinux container — not needed on macOS / Windows). Wheel output lands at `build/wheelhouse/pylibseekdb-*.whl`. Each release produces **6 wheels** (3 platforms × 2 Python ABIs): `cp311-cp311` for Python 3.11, and `cp312-abi3` for Python 3.12+ (including 3.13 and 3.14) on linux x86_64, linux aarch64, and macOS arm64. See `python/pyproject.toml`.

### One-shot wheel build script

`scripts/build-pylibseekdb-wheel.sh` orchestrates the full release flow: obtain a `seekdb` binary (local path, download URL, or source build), strip it while saving debug symbols, build `libseekdb`, and run `cibuildwheel`. By default it creates `.venv` in the repo root and installs `nanobind`, `cibuildwheel`, and `scikit-build-core` from `scripts/requirements-wheel-build.txt`. Use `--no-venv` or set `PYTHON=` to use a system interpreter instead.

```sh
# Prebuilt seekdb binary
./scripts/build-pylibseekdb-wheel.sh --seekdb-bin /path/to/seekdb

# Download a binary
./scripts/build-pylibseekdb-wheel.sh --seekdb-url https://example.com/seekdb

# Build seekdb from git, then package wheels (manylinux_2_28 on Linux)
./scripts/build-pylibseekdb-wheel.sh --build-seekdb \
  --seekdb-git-url https://github.com/oceanbase/seekdb.git \
  --seekdb-git-ref master

# Override wheel version and target one Linux arch
./scripts/build-pylibseekdb-wheel.sh --seekdb-bin /path/to/seekdb \
  --wheel-version 0.2.0 --platform linux --arch x86_64
```

`--wheel-version` temporarily patches `python/pyproject.toml` for the build (scikit-build-core does not read `CIBW_PROJECT_VERSION`) and restores it afterward.

Run `./scripts/build-pylibseekdb-wheel.sh --help` for all options. Debug symbols are written to `build/seekdb.debug` by default.

**Linux: build seekdb inside manylinux.** The wheel must be ABI-compatible with `manylinux_2_28`, so the `seekdb` binary you point `SEEKDB_BIN` at also needs to be built against glibc 2.28. `scripts/build-seekdb-glibc228.sh` runs `seekdb`'s own `build.sh` inside the manylinux image:

```sh
SEEKDB_REPO=~/seekdb ./scripts/build-seekdb-glibc228.sh

# Or clone a specific tag inside the container (binary exported to ./build/seekdb-glibc228):
SEEKDB_GIT_URL=https://github.com/oceanbase/seekdb.git \
SEEKDB_GIT_REF=v1.0.0 ./scripts/build-seekdb-glibc228.sh

# Or choose an explicit export path:
SEEKDB_GIT_URL=https://github.com/oceanbase/seekdb.git \
SEEKDB_GIT_REF=v1.0.0 \
SEEKDB_OUT_BIN=./build/seekdb ./scripts/build-seekdb-glibc228.sh

# Optional seekdb cmake flags (forwarded to seekdb's build.sh before --make):
./scripts/build-seekdb-glibc228.sh release \
  --seekdb-cmake-arg -DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN

./scripts/build-pylibseekdb-wheel.sh --build-seekdb \
  --seekdb-cmake-arg -DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN
```

Docker (or Podman if Docker is unavailable) pulls the host-matching `manylinux_2_28` image (x86_64 or aarch64). Set `CONTAINER_RUNTIME=podman` to force Podman. It prints the resulting binary path and the max GLIBC symbol version at the end — that's the path to use as `SEEKDB_BIN` before `make wheel`.

Each wheel built by `cibuildwheel` is smoke-tested with
`python/tests/connection_options_test.py`, which covers the native hybrid-search
path plus PyMySQL and aiomysql over the local endpoint. To verify wheels built
another way:

```sh
./scripts/verify-wheel.sh build/wheelhouse/pylibseekdb-*.whl
```

Override the test script with `SEEKDB_TEST_PY=/path/to/seekdb_test.py` if needed.
