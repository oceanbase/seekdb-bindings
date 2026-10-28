# Embedded native runtime decision

Status: implemented with pinned 1.4.0.0-r2 release assets for GNU/Linux
x86_64/ARM64, macOS Intel/ARM64, and Windows x86_64 MSVC (preview). See
[native validation](native-bundle-validation.md) for evidence and remaining limits.

## Decision

Embed the compressed native seekdb bundle in compiled artifacts that use the
binding. During a build, `build.rs` downloads the bundle for the target and
verifies its pinned SHA-256. `[package.metadata.seekdb]` pins the native
version and the SHA-256 of its release manifest. The build obtains that exact
manifest from verified local copies or the configured HTTPS download directory;
on a transport failure it can use the matching packaged JSON snapshot. Invalid
remote metadata is an error. `SEEKDB_DOWNLOAD_BASE_URL` overrides the build-time
directory for both metadata and archives while retaining all pins. The binding includes the compressed native archive with
`include_bytes!`. Before loading libseekdb or opening an instance that may start
a server, ensure a complete extraction exists. Load libseekdb by its absolute
path. Runtime extraction needs no download.

The archive is a build output, not a file added to the source package published
to crates.io. A downstream library normally refers to the seekdb Rust dependency;
publishing that library does not automatically publish the downloaded archive.
The final application or native library includes the embedded bytes when it
links the binding's runtime code.

## One cache for builds and runtime

Use the same `<root>/<target>/<native-version>/<full-sha256>` entry for both
phases. Keep the verified source archive as `bundle.tar.gz` alongside the
extracted native bundle and `.complete` manifest. Builds and runtime share the
same implementation, validation, and atomic directory publication. Build-time
manifest metadata uses `<root>/manifests/<native-version>/<manifest-sha256>/manifest.json`
and an independent `OUT_DIR` copy, publishing complete files by atomic rename.
Runtime uses its embedded metadata/archive and never accesses a manifest URL.
Each process
uses a private staging directory next to the final entry. It renames a complete
entry into place or reuses another process's winning entry; no cache file lock
is used. Linux/macOS replace damaged directories with atomic exchange. Windows
publishes a retained generation and atomically replaces its inventory pointer;
its entry and previous loaded files stay present throughout repair. Concurrent cold starts may duplicate downloads and extraction.
Building prepares files without loading the library or launching the server.
A subsequent run with the same user, cache root, and bundle identity reuses the
prepared native files directly. The compressed copy is not a required runtime
file and can be repaired independently without replacing mapped libraries.

Use the platform default described below, or `SEEKDB_CACHE_DIR` during both
build and runtime. The Rust API can override the runtime root; build.rs cannot
know an application's future API configuration. With a different root or on a
different machine, runtime uses the embedded archive to prepare its own entry.

Copy the verified archive into `OUT_DIR` before embedding, rather than making
the compiler depend on a user-cache path that can be cleaned independently.
An existing valid `OUT_DIR` archive can populate the shared entry offline.
Build-time cache preparation is best effort and falls back to `OUT_DIR` with a
warning if unavailable. Building needs writable storage, while native execution
and its `noexec` check occur at runtime.

Continue embedding the archive in compiled applications and libraries. Accept
the artifact size cost for now; reconsider the delivery strategy if measured
application size or deployment needs justify a change.

## Extraction location

Use a per-user cache with an entry identified by the target triple, native
version, and full archive SHA-256:

```text
Linux:
  $XDG_CACHE_HOME/seekdb/native/<target>/<version>/<sha256>/
  or ~/.cache/seekdb/native/<target>/<version>/<sha256>/

macOS:
  ~/Library/Caches/seekdb/native/<target>/<version>/<sha256>/

Windows:
  %LOCALAPPDATA%\seekdb\cache\native\<target>\<version>\<sha256>\
```

Use platform directory APIs or a cross-platform directory library to discover
these locations. On Linux, use `XDG_CACHE_HOME` only when it is an absolute path;
otherwise fall back to `~/.cache`. On Windows, resolve the LocalAppData known
folder rather than constructing a user profile path manually. These directory
conventions do not add support for native targets without configured bundles.

### Cache root configuration

Allow an explicit cache root through both a Rust API and the cross-platform
`SEEKDB_CACHE_DIR` environment variable. Resolve the root in this order:

1. Explicit Rust API configuration.
2. `SEEKDB_CACHE_DIR`.
3. The platform default shown above, including Linux's `XDG_CACHE_HOME` rule.

The override is the root immediately above `<target>/<version>/<sha256>`.
For example, `SEEKDB_CACHE_DIR=/data/seekdb-cache` selects
`/data/seekdb-cache/<target>/<version>/<sha256>/`. This lets an operator change
the extraction location without rebuilding an application or requiring its own
configuration interface. An unusable explicit override must produce an error;
do not silently select a different directory.

Configure the root before the first native initialization. Freeze it after
successful initialization for that copy of the seekdb Rust dependency; an API
call that tries to change it afterward must fail clearly. Libraries B and C
sharing that dependency therefore use the application-selected location. Each
subsequent open still checks and repairs the extraction at the selected path.

`SEEKDB_CACHE_DIR` selects the shared build/runtime bundle cache.
The Rust API is `seekdb::set_runtime_cache_dir(absolute_path)`. Explicit API and
environment overrides must be non-empty absolute paths.

Database data stays in the directory explicitly selected by the caller, outside
this disposable runtime cache.

Preserve the bundle's directories, permissions, symlinks, and relative native
loader paths. Extract into a private staging directory, validate the required
files, and publish the completed entry through rename. A completion
marker alone is insufficient if required files have subsequently disappeared.
Compatible callers can reuse the same completed cache entry.

Repairing a nonempty damaged directory uses Linux `renameat2(RENAME_EXCHANGE)`
or macOS `renamex_np(RENAME_SWAP)`. This also handles a stale repair decision:
if another publisher installed a healthy entry in the meantime, exchanging two
complete copies keeps the final path available. Preserve the replaced directory
for processes that still map its files. A filesystem without atomic-exchange
support reports a cache error during repair; use another cache filesystem or
remove the damaged entry when no instance uses it. Keep archive staging files
beside the entry so directory exchange cannot invalidate their source paths.

Windows has no atomic directory-exchange API. For a damaged entry, rename the
prepared sibling tree into a unique generation within the entry, then atomically
replace `.complete` with an inventory referencing only that complete generation.
Keep all prior files and generations. A stale publisher can replace the inventory
with another complete generation without removing paths already used by readers.
The loaded driver is retained; its existing `seekdb_set_binary_path` C API selects
the server in the latest generation before another open. Windows DLL loading uses
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, and a
server copy is colocated with the bundled DLLs. No global loader path changes or
SDK adapter DLL are required. The long archive wrapper directory is removed on
Windows to keep paths within the pinned ANSI C driver's limits.

This generation scheme changes only Windows repair; Unix continues to use the
existing atomic-exchange scheme. Windows filesystem/DACL policy is enforced by
the OS; custom cache roots must remain private to the user. The pinned C driver
limits native paths to those representable in the active ANSI code page and short
local-drive paths. The Rust layer rejects unsupported paths explicitly.

## Accepted risks and implementation requirements

Embedding provides a distributable artifact containing the native bundle, but
seekdb still runs from extracted files and launches a separate server process.
It therefore has these deployment constraints:

| Risk | Required handling |
| --- | --- |
| The default cache is read-only, on a `noexec` mount, or blocked by execution policy. | Allow a caller-selected writable cache through the Rust API or `SEEKDB_CACHE_DIR`. On Linux, diagnose a confirmed `noexec` mount before native initialization, for example using `statvfs` flags. Report the actual path, failed operation, and underlying system error; do not classify every permission failure as `noexec`. Explain how to select a different cache root. Embedding cannot bypass host execution policy. |
| The cache is cleared before use or an extraction is interrupted. | Treat this as a recoverable cache miss: recreate the missing or incomplete entry from the embedded bytes without a download. Check before each `Seekdb::open` that may start a server, even when libseekdb is already loaded. The cost is extraction time and disk writes; database data is outside this cache. |
| An external cleaner deletes cache files while an instance is running. | On Linux, unlinking an executable or shared library does not invalidate its existing memory mappings, so deletion alone generally does not stop an already running server. Later file opens, deferred library loads, or server starts can still require the deleted paths. Restore the bundle before operations controlled by the binding; do not promise transparent recovery from arbitrary deletion during native execution. SDK-managed cleanup must avoid entries in use and must never overwrite active mapped files in place. |
| Several applications initialize the same bundle concurrently. | Prepare independently and publish complete entries with rename, reusing an already valid winner. Accept duplicate cold-start work. Use Unix directory exchange or Windows generation/inventory publication, preserving paths held by concurrent readers. |
| Cache contents are modified by another user. | Use a user-owned cache with appropriate access permissions and safely extract archive paths. A completion marker is not an integrity guarantee. |
| Native artifacts increase application size, first-use latency, and disk usage. | Document that the compressed archive remains in the compiled artifact and the extracted runtime consumes additional disk space. Different versions can leave separate cache entries. |
| Different SDK versions or separately built modules embed multiple archives. | Keep cache entries distinct by target, native version, and archive hash. Cargo can share a resolved Rust dependency within one build graph, but cannot deduplicate payloads across independently built dynamic libraries. |
| The native bundle requires a particular operating system, architecture, or glibc version. | Retain explicit platform requirements. Embedding does not remove native ABI or system dependency requirements. |

The writable and executable extraction directory requirement is an accepted
tradeoff of this decision and must appear in the user-facing deployment guide
alongside the deployment instructions.
