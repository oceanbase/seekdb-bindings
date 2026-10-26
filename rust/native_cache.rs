use flate2::read::GzDecoder;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, PermissionsExt};
use std::path::{Component, Path, PathBuf};

pub(crate) const ARCHIVE_NAME: &str = "bundle.tar.gz";

pub(crate) struct NativeCache<'a> {
    pub root: PathBuf,
    pub hash: &'a str,
    pub target: &'a str,
    pub version: &'a str,
    pub library: &'a str,
}

#[derive(Debug)]
pub(crate) struct CacheError {
    pub path: PathBuf,
    pub operation: &'static str,
    pub source: io::Error,
}

impl std::fmt::Display for CacheError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            formatter,
            "{} at {}: {}",
            self.operation,
            self.path.display(),
            self.source
        )
    }
}

pub(crate) fn default_root() -> Option<PathBuf> {
    dirs::cache_dir().map(|root| {
        let root = root.join("seekdb");
        let root = if cfg!(windows) {
            root.join("cache")
        } else {
            root
        };
        root.join("native")
    })
}

fn cache_error(path: &Path, operation: &'static str, source: io::Error) -> CacheError {
    CacheError {
        path: path.to_path_buf(),
        operation,
        source,
    }
}

#[derive(Deserialize, Serialize)]
struct Manifest {
    hash: String,
    native_dir: PathBuf,
    entries: Vec<Entry>,
}

#[derive(Deserialize, Serialize)]
struct Entry {
    path: PathBuf,
    kind: Kind,
}

#[derive(Deserialize, Serialize)]
enum Kind {
    Directory,
    File { length: u64, executable: bool },
    Symlink { target: PathBuf },
}

fn private_directory(path: &Path) -> Result<(), CacheError> {
    fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(path)
        .map_err(|error| cache_error(path, "create cache directory", error))?;
    let metadata =
        fs::metadata(path).map_err(|error| cache_error(path, "inspect cache directory", error))?;
    // SAFETY: geteuid has no preconditions and does not change process state.
    if metadata.uid() != unsafe { libc::geteuid() } || metadata.mode() & 0o022 != 0 {
        return Err(cache_error(
            path,
            "inspect cache directory",
            io::Error::new(
                io::ErrorKind::PermissionDenied,
                "cache directory must be owned by the current user and not writable by other users",
            ),
        ));
    }
    Ok(())
}

fn relative(path: &Path) -> bool {
    !path.as_os_str().is_empty()
        && path
            .components()
            .all(|part| matches!(part, Component::Normal(_)))
}

fn native_directory(root: &Path, library: &str) -> io::Result<PathBuf> {
    let mut candidates = vec![root.join("lib")];
    for entry in fs::read_dir(root)? {
        let entry = entry?;
        if entry.file_type()?.is_dir() {
            candidates.push(entry.path().join("lib"));
        }
    }
    let mut matches = candidates
        .into_iter()
        .filter(|path| path.join(library).is_file() && path.join("seekdb").is_file());
    let directory = matches.next().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("bundle is missing a lib/ directory containing {library} and seekdb"),
        )
    })?;
    if matches.next().is_some() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "bundle contains multiple native lib/ directories",
        ));
    }
    if fs::metadata(directory.join("seekdb"))?.permissions().mode() & 0o111 == 0 {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "bundled seekdb is not executable",
        ));
    }
    Ok(directory)
}

fn inventory(root: &Path, directory: &Path, entries: &mut Vec<Entry>) -> io::Result<()> {
    for item in fs::read_dir(directory)? {
        let item = item?;
        let path = item.path();
        let metadata = fs::symlink_metadata(&path)?;
        let kind = if metadata.file_type().is_symlink() {
            if !path.canonicalize()?.starts_with(root) {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "bundle symlink escapes extraction directory",
                ));
            }
            Kind::Symlink {
                target: fs::read_link(&path)?,
            }
        } else if metadata.is_dir() {
            inventory(root, &path, entries)?;
            Kind::Directory
        } else if metadata.is_file() {
            Kind::File {
                length: metadata.len(),
                executable: metadata.permissions().mode() & 0o111 != 0,
            }
        } else {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "unsupported file type in native bundle",
            ));
        };
        entries.push(Entry {
            path: path.strip_prefix(root).unwrap().to_path_buf(),
            kind,
        });
    }
    Ok(())
}

impl NativeCache<'_> {
    pub(crate) fn entry(&self) -> PathBuf {
        self.root
            .join(self.target)
            .join(self.version)
            .join(self.hash)
    }

    pub(crate) fn archive_path(&self) -> PathBuf {
        self.entry().join(ARCHIVE_NAME)
    }

    pub(crate) fn prepare_directory(&self) -> Result<(), CacheError> {
        private_directory(&self.root)?;
        private_directory(self.entry().parent().unwrap())
    }

    fn valid_cache(&self, root: &Path) -> io::Result<PathBuf> {
        let canonical_root = root.canonicalize()?;
        let mut bytes = Vec::new();
        File::open(root.join(".complete"))?
            .take(1024 * 1024)
            .read_to_end(&mut bytes)?;
        let manifest: Manifest = serde_json::from_slice(&bytes)?;
        if manifest.hash != self.hash
            || !relative(&manifest.native_dir)
            || manifest.entries.is_empty()
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid native cache manifest",
            ));
        }
        for entry in &manifest.entries {
            if !relative(&entry.path) {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "invalid path in native cache manifest",
                ));
            }
            let path = root.join(&entry.path);
            let metadata = fs::symlink_metadata(&path)?;
            let valid = match &entry.kind {
                Kind::Directory => metadata.is_dir(),
                Kind::File { length, executable } => {
                    metadata.is_file()
                        && metadata.len() == *length
                        && (!executable || metadata.permissions().mode() & 0o111 != 0)
                }
                Kind::Symlink { target } => {
                    metadata.file_type().is_symlink() && fs::read_link(&path)? == *target
                }
            };
            if !valid || !path.canonicalize()?.starts_with(&canonical_root) {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "native cache entry is incomplete",
                ));
            }
        }
        let native = native_directory(root, self.library)?;
        if native != root.join(manifest.native_dir) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "native cache layout changed",
            ));
        }
        Ok(native.join(self.library))
    }

    /// Prepare independently, then publish or reuse another process's complete entry.
    pub(crate) fn ensure(&self, archive: &[u8]) -> Result<PathBuf, CacheError> {
        self.prepare_directory()?;
        let expanded = self.entry();
        if let Ok(path) = self.valid_cache(&expanded) {
            return Ok(path);
        }
        self.verify_archive(archive)?;
        let parent = expanded.parent().unwrap();
        let staging = tempfile::Builder::new()
            .prefix(".extract-")
            .tempdir_in(parent)
            .map_err(|error| cache_error(parent, "create staging directory", error))?;
        tar::Archive::new(GzDecoder::new(archive))
            .unpack(staging.path())
            .map_err(|error| cache_error(staging.path(), "extract native archive", error))?;
        for reserved in [ARCHIVE_NAME, ".complete"] {
            if staging.path().join(reserved).exists() {
                return Err(cache_error(
                    staging.path(),
                    "validate native layout",
                    io::Error::new(
                        io::ErrorKind::InvalidData,
                        "native archive contains a reserved cache filename",
                    ),
                ));
            }
        }
        let native = native_directory(staging.path(), self.library)
            .map_err(|error| cache_error(staging.path(), "validate native layout", error))?;
        let canonical = staging
            .path()
            .canonicalize()
            .map_err(|error| cache_error(staging.path(), "resolve staging directory", error))?;
        let mut entries = Vec::new();
        inventory(&canonical, &canonical, &mut entries)
            .map_err(|error| cache_error(staging.path(), "inventory native bundle", error))?;
        let manifest = Manifest {
            hash: self.hash.into(),
            native_dir: native.strip_prefix(staging.path()).unwrap().into(),
            entries,
        };
        // The archive is a build input, not a required native runtime file. Keep
        // it out of the inventory so builds can repair it without replacing .so files.
        self.store_archive(staging.path(), archive)?;
        let marker = staging.path().join(".complete");
        let bytes = serde_json::to_vec(&manifest)
            .map_err(|error| cache_error(&marker, "serialize completion manifest", error.into()))?;
        fs::write(&marker, bytes)
            .map_err(|error| cache_error(&marker, "write completion manifest", error))?;
        self.publish(staging)
    }

    fn publish(&self, staging: tempfile::TempDir) -> Result<PathBuf, CacheError> {
        let expanded = self.entry();
        for _ in 0..8 {
            if let Ok(path) = self.valid_cache(&expanded) {
                return Ok(path); // A concurrent publisher won; drop our private staging tree.
            }
            match fs::rename(staging.path(), &expanded) {
                Ok(()) => {
                    return self
                        .valid_cache(&expanded)
                        .map_err(|error| cache_error(&expanded, "validate published cache", error))
                }
                Err(error) => {
                    if let Ok(path) = self.valid_cache(&expanded) {
                        return Ok(path);
                    }
                    match fs::symlink_metadata(&expanded) {
                        Err(missing) if missing.kind() == io::ErrorKind::NotFound => continue,
                        Err(_) => {
                            return Err(cache_error(&expanded, "publish native cache", error))
                        }
                        Ok(_) => {}
                    }
                }
            }
            // Replacing a nonempty directory needs an atomic exchange. Two plain
            // renames would leave a gap and could retire a concurrent winner.
            match exchange_directories(staging.path(), &expanded) {
                Ok(()) => {
                    // Staging now holds the previous entry. Keep its mapped files
                    // alive even if another publisher just replaced a healthy entry.
                    let old = staging.keep();
                    let parent = expanded.parent().unwrap();
                    if let Ok(retired) = tempfile::Builder::new()
                        .prefix(".retired-")
                        .tempdir_in(parent)
                    {
                        if fs::rename(&old, retired.path().join("bundle")).is_ok() {
                            let _ = retired.keep();
                        }
                    }
                    return self
                        .valid_cache(&expanded)
                        .map_err(|error| cache_error(&expanded, "validate replaced cache", error));
                }
                Err(error) if error.kind() == io::ErrorKind::NotFound => continue,
                Err(error) => {
                    return Err(cache_error(
                        &expanded,
                        "atomically replace incomplete cache",
                        error,
                    ))
                }
            }
        }
        Err(cache_error(
            &expanded,
            "publish native cache",
            io::Error::other("cache directory repeatedly disappeared during publication"),
        ))
    }

    fn verify_archive(&self, archive: &[u8]) -> Result<(), CacheError> {
        if format!("{:x}", Sha256::digest(archive)) != self.hash {
            return Err(cache_error(
                &self.archive_path(),
                "verify native archive",
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    "native seekdb archive SHA-256 mismatch",
                ),
            ));
        }
        Ok(())
    }

    /// Publish only the compressed copy, leaving native files intact.
    pub(crate) fn store_archive(&self, entry: &Path, archive: &[u8]) -> Result<(), CacheError> {
        self.verify_archive(archive)?;
        let path = entry.join(ARCHIVE_NAME);
        // Keep the staging file beside the cache entry, because another process
        // may exchange the entry directory while this file is being written.
        let parent = entry.parent().unwrap();
        let mut temporary = tempfile::NamedTempFile::new_in(parent)
            .map_err(|error| cache_error(parent, "create archive staging file", error))?;
        temporary
            .write_all(archive)
            .map_err(|error| cache_error(&path, "write cached archive", error))?;
        temporary
            .persist(&path)
            .map_err(|error| cache_error(&path, "publish cached archive", error.error))?;
        Ok(())
    }
}

fn exchange_directories(left: &Path, right: &Path) -> io::Result<()> {
    let left = std::ffi::CString::new(left.as_os_str().as_bytes())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "cache path contains NUL"))?;
    let right = std::ffi::CString::new(right.as_os_str().as_bytes())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "cache path contains NUL"))?;
    #[cfg(target_os = "linux")]
    // SAFETY: Both paths are NUL-terminated. renameat2 atomically exchanges names
    // on the same filesystem and never modifies files that other processes map.
    let result = unsafe {
        libc::renameat2(
            libc::AT_FDCWD,
            left.as_ptr(),
            libc::AT_FDCWD,
            right.as_ptr(),
            libc::RENAME_EXCHANGE,
        )
    };
    #[cfg(target_os = "macos")]
    // SAFETY: Both paths are valid C strings; RENAME_SWAP provides the same exchange.
    let result = unsafe { libc::renamex_np(left.as_ptr(), right.as_ptr(), libc::RENAME_SWAP) };
    #[cfg(not(any(target_os = "linux", target_os = "macos")))]
    return Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "atomic cache directory exchange is unsupported on this platform",
    ));
    #[cfg(any(target_os = "linux", target_os = "macos"))]
    if result == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}
