# SeekDB Swift bindings

`Sources/SeekDB` contains the API (`SeekDB`, `ConnectionOptions`, `SeekDBError`).
It runs seekdb in process: `NativeAPI.swift` calls the lifecycle functions of
oceanbase/seekdb's `SeekDB.framework` through the `CSeekDB` module, and the framework
runs the engine inside the application on a dedicated 8 MiB-stack thread. An iOS app
cannot start the separate SeekDB process that libseekdb launches on desktop systems
and Android. The API follows the Java bindings. It has no third-party Swift
dependencies and does not include a SQL client, construct connection URLs, execute
queries, or decode result sets. Applications choose and configure their own
MySQL-protocol client; the [iOS example App](example/README.md) uses MySQLNIO.

## Package layout

This directory is a SwiftPM package: `Package.swift` exposes the `SeekDB` library
from `Sources/SeekDB`, with tests in `Tests/SeekDBTests`. The engine is seekdb's
`SeekDB.framework`, which `build.sh` in this directory packages as
`build/SeekDBEngine.xcframework` next to it (see [Build](#build)); when that exists,
the package adds it as the binary target `SeekDBEngine`. It holds iOS slices only for now.

`CSeekDB` is a module map whose `shim.h` includes the framework's own
`<SeekDB/seekdb.h>`. That header comes from seekdb's
`src/observer/in_process/seekdb.h` and declares `seekdb_open`,
`seekdb_last_open_error`, `seekdb_close` and `seekdb_connection_options`. The
repository's `lib/include/seekdb/seekdb.h` belongs to the out-of-process `libseekdb`
and is not used here.

`NativeAPI.swift` imports `CSeekDB` with `@_implementationOnly`. With a plain or
`internal import`, every module that imports `SeekDB` must also find the `CSeekDB`
module map, because clients of a module built without library evolution load its
imports, and compiling without it fails with "missing required module 'CSeekDB'".
The `SeekDB` target enables the experimental feature `CheckImplementationOnly`, which
makes storing a `CSeekDB` type in a property a compile error rather than a warning:
clients would compute such a type's layout without seeing `CSeekDB`.

```swift
// In a consuming package's Package.swift:
dependencies: [
    .package(path: "../seekdb-bindings/swift")
],
targets: [
    .target(name: "MyApp", dependencies: [
        .product(name: "SeekDB", package: "swift")
    ])
]
```

SwiftPM names a package added by path after its directory, hence `package: "swift"`.
It reads a Git-URL dependency's `Package.swift` only from the repository root, so
this package is used by path; Git-URL consumption will need a separate repository
with its own root manifest. A native engine is still required, just as the Java
JAR requires its matching native components. The package does not download
engines or change process search paths.

## Build

Requires macOS with full Xcode and the iOS SDKs. `build.sh` uses
`/Applications/Xcode.app/Contents/Developer` unless `DEVELOPER_DIR` is set. Build
the framework in a seekdb checkout, then package it:

```sh
cd /path/to/seekdb
./build.sh release --ios --framework --init --make -j8               # device
./build.sh release --ios --simulator --framework --init --make -j8   # Simulator
cd /path/to/seekdb-bindings
SEEKDB_DIR=/path/to/seekdb bash swift/build.sh
```

`build.sh` puts each `SeekDB.framework` it finds in `SEEKDB_DIR` (device, Simulator
or both) into `build/SeekDBEngine.xcframework` in this directory, after checking each
framework's platform. `BUILD_SWIFT_PACKAGE=1` also builds this package with
`xcodebuild` for each packaged platform, and `TEST_SIMULATOR=<simulator id>` runs the
tests on that Simulator; their intermediate files go to `.build/xcode` here.

## API

```swift
import SeekDB

// Run this work away from the UI thread: open and close are synchronous.
let db = try SeekDB.open("/absolute/writable/persistent/database")
defer { try? db.close() }

let options = try db.connectionOptions()
// Configure your chosen client using options.transport and its endpoint fields.
// Execute SQL and close all client connections before closing db.

try db.close()
```

`ConnectionOptions` is a copied, immutable, `Sendable` value with fields matching
the C and Java APIs: `transport`, `port`, `host`, `unix_socket`, `named_pipe`, and
`user`. Unused strings remain `nil`; local endpoints have port zero. The snapshot
does not extend the native handle's lifetime. Do not open client connections from
saved options after the owning instance closes.

`SeekDB.open(path, parameters: ["memory_budget", "2G", "log_disk_size", "4G"])`
passes alternating key/value strings to `seekdb_open`. The wrapper rejects
odd-length lists, empty names, and embedded NUL bytes. Other values are validated
by the native implementation. Native open errors retain their return code and
same-thread diagnostic in `SeekDBError`.

Options and close operations on a handle are serialized. A successful close is
idempotent, and options requested afterward throw `SeekDBError.closed`. Failed
close retains the handle. `deinit` attempts best-effort cleanup, but explicit close
is required to observe failures.

## Runtime constraints

Xcode embeds and signs `SeekDB.framework` with an app that depends on `SeekDB`. Apps
that call the C API directly can embed the framework and include its
`Headers/seekdb.h`.

- iOS 18 or newer. Device and Simulator frameworks are separate slices.
- The first open starts the engine, which keeps running until the process exits;
  close only releases the handle. Opening the same directory again reuses the
  running engine, also after every handle has been closed. Another directory is
  rejected, and so is any open after the engine failed to start;
  `seekdb_last_open_error` says why.
- A new database defaults to `memory_budget=1G`, `vector_memory_limit=128M`,
  `log_disk_size=2G` and `datafile_maxsize=20G`; parameters override them when the
  database is created. TCP is not available: a non-zero `port`, or a
  `mysql_port_mode` other than `disabled`, is rejected.
- Open returns once the engine runs and its Unix socket sends a MySQL handshake.
  The connection options carry `transport` `unix_socket`, the socket path in
  `unix_socket` (a short alias under the App's tmp directory, or under `/tmp` on
  the Simulator, when the real path is too long) and `user` `root`, with an empty
  password.
- The engine sets the process working directory to its data directory and leaves
  it there, so use absolute paths. The first open blocks until the engine accepts
  connections; call it away from the UI thread.

## Tests

`Tests/SeekDBTests` covers public validation, native diagnostics, copied UTF-8
values, closed-handle access, idempotence, failed-close ownership, deinitialization
and concurrent options/close. `testInProcessEngineKeepsServingAfterClose` starts the
engine, checks the connection options, reads the MySQL handshake from the socket,
closes the handle, opens the same directory again and reads the handshake once more,
and checks that another directory is refused with its diagnostic. The tests run on a
Simulator through `TEST_SIMULATOR`.

Validated on 2026-10-15 with macOS 27.0, Xcode 27.0, Swift 6.4 and the iOS 27.0
SDK, against Device and Simulator frameworks built from seekdb `9786f15d9` (branch
`feature/ios-swift`, 4 exports), with the package reading the framework's own
header: `build.sh` packaged both and built the package for each with
`BUILD_SWIFT_PACKAGE=1`, and the 14 tests passed on an iPhone 17 Simulator with
iOS 27.0, the in-process engine test in 1.0 second (about 14 seconds before, when
close still stopped the engine). On the same Simulator, a separate Swift package
that depends on this one by local path opened the engine through the `SeekDB`
product, read its connection options and closed it, and neither test process
crashed when it exited with the engine still running. The Device framework was
built and verified but not run on a device.

## Release

`release.sh` prepares a release of this package in a checkout of
[cao1629/seekdb-swift](https://github.com/cao1629/seekdb-swift), the repository that
apps add by URL. Package the device and Simulator frameworks with `build.sh` from a
clean seekdb tree first, then:

```sh
git clone https://github.com/cao1629/seekdb-swift.git ~/repo/seekdb-swift
swift/release.sh 0.1.0 ~/repo/seekdb-swift
```

It checks that both slices come from the same seekdb revision built without
uncommitted changes (`ALLOW_DIRTY=1` allows a trial run), zips the XCFramework
without macOS `._` files into `build/release/<version>/`, computes the archive's
checksum, and writes `Package.swift` (whose `binaryTarget(url:checksum:)` points at
the GitHub Release asset), `Sources/`, `README.md`, `LICENSE` and `.gitignore` into
the checkout. It then prints the commit, tag, push and `gh release create` commands
and publishes nothing itself. Upload exactly the archive it made, since the checksum
belongs to that file. `RELEASE_REPOSITORY` names another repository.

Versions stay at 0.x while the engine comes from an unreleased seekdb branch; once
a seekdb release ships the in-process runtime, the package takes that release's
version.
