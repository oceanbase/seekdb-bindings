# iOS example App

This directory is a SwiftUI App that uses the Swift package in `swift/` the way an
application would. It depends on that package and on the
`swift/build/SeekDBEngine.xcframework` that `swift/build.sh` packages from seekdb's
`SeekDB.framework`; the [Swift bindings](../README.md#build) describe how to
build both, and the runtime constraints apps have to follow.

On launch the App opens a database in its `Documents/seekdb` directory with
`SeekDB.open`, connects to the Unix socket from `connectionOptions()` with
[MySQLNIO](https://github.com/vapor/mysql-nio), a pure-Swift MySQL client, runs
`SELECT 1`, closes the handle and writes `{"select_1":1}` to `Documents/select1.json`,
or `{"error": ...}` when a step fails. MySQLNIO always assumes the
`CLIENT_DEPRECATE_EOF` capability, which seekdb does not announce, so its own
`simpleQuery` returns no rows from seekdb; `Sources/TextQuery.swift` is a
`MySQLCommand` that reads a text result set either way.

The Xcode project is generated from `project.yml` with
[XcodeGen](https://github.com/yonaskolb/XcodeGen). It depends on the package in
`swift/` by local path and on MySQLNIO and SwiftNIO from GitHub, and it signs ad hoc
for the Simulator; to run on a device, open the generated `SeekDBExample.xcodeproj`
in Xcode and choose a team. `run.sh` generates the project, builds the App for
a Simulator into `build/`, installs and launches it, and waits up to 300
seconds for `select1.json`; it fails unless the result is 1. Package the framework
with `swift/build.sh` first.

```sh
brew install xcodegen
bash swift/example/run.sh SIMULATOR_UDID
```

Validated on 2026-10-15 on an iPhone 17 Simulator with iOS 27.0, against frameworks
built from seekdb `9786f15d9`: the App returned `{"select_1":1}` on a fresh database
and again in a new process on the existing one. It has not been run on a device.

The example replaces `framework_probe/`, an Objective-C++ App that loaded the
framework's C functions with `dlopen` and ran SQL through MariaDB Connector/C, with
its validators, stability runner and recorded runs; git history keeps it.
