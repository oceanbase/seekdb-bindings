# Native runtime redistribution

The Rust binding's `license = "Apache-2.0"` describes its own source code;
it does not relicense bundled third-party code. See [LICENSE](LICENSE).

Normal builds embed the complete verified native runtime archive in the user's
application or cdylib, including its `licenses/` directory. Runtime extraction
preserves those files. Distributors must retain the required licenses/notices
and provide them with their product as required by the applicable licenses;
embedding bytes or using a private extraction cache does not remove those
obligations. The source `.crate` contains no native binaries.

## Evidence from the pinned r2 archives

All five runtime archives were checked against the sizes and SHA-256 values in
[the release manifest](bundles/manifest-1.4.0.0-r2.json). Each contains the bindings
Apache-2.0 license, the engine Apache-2.0 license and the engine NOTICE. Filename
prefixes differ (`seekdb-src-`, `seekdb-src-local-`, or `seekdb-`). The license
contents agree across platforms. Their SHA-256 is
`c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4`.
The engine NOTICE has SHA-256
`c753aec56c61055ae25ca41d71984674b1f32d2e1c745efbbaa5bdbaf3ab2a0b`;
an exact copy is included as [NOTICE](NOTICE) in this crate. It contains notices
for libev, http_parser, LZ4 (two versions), snappy, zlib and zstd.

That NOTICE is not a complete inventory of the prebuilt runtime. In particular:

| Area | Observed dependency/source | Distribution work to verify |
| --- | --- | --- |
| C interface library | The pinned bindings build links `mariadbclient`; [its pinned source license](https://github.com/mariadb-corporation/mariadb-connector-c/blob/46880b003653a000e9588bd73c8b1dd65088c686/COPYING.LIB) is LGPL-2.1 | Connector notices and applicable source/relink requirements for the actual linked build |
| GNU/Linux | Runtime archives also ship `libaio.so.1` | The shipped libaio version, license and required materials |
| macOS Intel | Runtime archive also ships `libomp.dylib` | The shipped OpenMP runtime version and notices |
| macOS ARM64 | Runtime archive also ships `libcrypto.3.dylib` and `libssl.3.dylib` | The shipped OpenSSL version and notices |
| Windows | Dependency DLLs include OpenSSL, OpenMP, Abseil, c-ares, iconv, ICU, curl, protobuf/protobuf-c, libxml2, pthreads, RE2, SQLite, zlib and MSVC runtime DLLs | Exact versions, license texts/notices and applicable redistribution terms, including Microsoft's runtime terms |

The engine and other libraries also contain statically linked dependencies; a
list of shared-library filenames alone cannot establish a complete inventory.
Use the native producer's build records and corresponding source to finish the
inventory and obtain the required materials before publication/distribution.
Do not assume that the three existing bundle notice files cover all those
components, or that this file grants redistribution rights.

The manifest identifies engine source commit
`bf450b4c8deb3c45d85efb1bd5f43e282d101914` and bindings source commit
`74034975b164e7df5b2ff48b642d2bb36853f6b3`. Native packaging revision,
per-platform provenance and archive hashes must remain associated with any
completed notice/source inventory. Fixing a published bundle's contents needs a
new verified native revision; do not alter r2 files or their expected hashes.
