# seekdb-bindings

C driver library, CLI, and Python bindings for [seekdb](https://github.com/oceanbase/seekdb).

## Layout

```
seekdb-bindings/
├── driver/
│   ├── include/seekdb.h        public C API
│   ├── src/                    library + CLI sources
│   └── tests/                  gtest cases
├── python/                     pybind11 module + cibuildwheel config
├── scripts/                    helper scripts (e.g. openssl for android)
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

All builds go to a single CMake build directory — `build/` at the repo root by convention, but any location works. The `SEEKDB_BIN` env var must point to a built `seekdb` binary — it's copied next to `libseekdb_driver.so` so the CLI / Python bindings can use it.

After configuration (per-platform commands below), choose a `make` target from inside `build/`:

| target | output |
|---|---|
| `make seekdb_driver` | `libseekdb_driver.so` (shared) |
| `make seekdb_cli` | `seekdb_cli` (interactive SQL client) |
| `make pylibseekdb` | `pylibseekdb.cpython-*.so` (Python extension) |
| `make seekdb_driver_tests` | `test_one_client_process`, `test_two_clients_threads` |
| `make wheel` | `wheelhouse/pylibseekdb-*.whl` via cibuildwheel |
| `make` | everything above |

`build/seekdb` (a copy of `$SEEKDB_BIN`) ships alongside the library.

### Linux

```sh
export SEEKDB_BIN=/path/to/seekdb
cmake -S . -B build
cd build
make seekdb_driver
```

### Mac

Identical to Linux, plus `-DWITH_EXTERNAL_ZLIB=YES` (the vendored zlib in mariadb-connector-c doesn't build cleanly on macOS):

```sh
export SEEKDB_BIN=/path/to/seekdb
cmake -S . -B build -DWITH_EXTERNAL_ZLIB=YES
cd build
make seekdb_driver
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
ninja seekdb_driver
```

Persist env vars across PowerShell sessions either via your `$PROFILE` file or with `[Environment]::SetEnvironmentVariable("SEEKDB_BIN", "C:\path\to\seekdb.exe", "User")`. The dev-shell load must be re-run every session — there's no good way to bake it in.

If running `.ps1` scripts is blocked by execution policy, allow them for the current process only:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
```

## CMake options

| option | default | what it does |
|---|---|---|
| `SEEKDB_DRIVER_ENABLE_LOG` | OFF | enables `tlog()` debug prints in the driver |
| `SEEKDB_BIN` | `$SEEKDB_BIN` env | path to the seekdb binary |

Example:

```sh
cmake -S . -B build -DSEEKDB_DRIVER_ENABLE_LOG=ON
cmake --build build
```

## Format source code

Install `clang-format-14` (or `clang-format` 14.x; see `.clang-format-version`),
configure the project as usual, then run:

```sh
cmake --build build --target format
```

The `format` target rewrites the repository's C, C++, and pybind11 source files using the
root `.clang-format` style.

## Use seekdb_cli

Interactive SQL shell, same shape as `mysql` / `mariadb`:

```sh
build/seekdb_cli [db_dir]
```

`db_dir` defaults to `./seekdb.db` (created if missing). The seekdb binary is auto-discovered next to `libseekdb_driver.so`.

## Prepare Python environment

`make pylibseekdb` and `make wheel` need a Python toolchain. Use a venv to keep `pybind11` (required by `make pylibseekdb` at configure time) and `cibuildwheel` (required by `make wheel`) out of the system Python.

### Linux / Mac

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install pybind11 cibuildwheel
```

### Windows (PowerShell)

```powershell
py -3 -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install pybind11 cibuildwheel
```

If `Activate.ps1` is blocked by execution policy, allow it for this process only:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
```

`cibuildwheel` additionally needs Docker running on Linux (it builds inside the manylinux container — not needed on macOS / Windows). Wheel output lands at `build/wheelhouse/pylibseekdb-*.whl`. Target platforms (`manylinux_2_28`, `macos arm64`, `win_amd64`) are configured in `python/pyproject.toml`.

**Linux: build seekdb inside manylinux.** The wheel must be ABI-compatible with `manylinux_2_28`, so the `seekdb` binary you point `SEEKDB_BIN` at also needs to be built against glibc 2.28. `scripts/build-seekdb-glibc228.sh` runs `seekdb`'s own `build.sh` inside the `manylinux_2_28_x86_64` image:

```sh
SEEKDB_REPO=~/seekdb ./scripts/build-seekdb-glibc228.sh    # defaults to ~/seekdb if SEEKDB_REPO is unset
```

It prints the resulting binary path and the max GLIBC symbol version at the end — that's the path to use as `SEEKDB_BIN` before `make wheel`.
