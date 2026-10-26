use crate::Error;
use flate2::read::GzDecoder;
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::ffi::OsString;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Component, Path, PathBuf};

mod embedded {
    include!(concat!(env!("OUT_DIR"), "/native_bundle.rs"));
}

pub(crate) struct Bundle<'a> {
    root: PathBuf,
    archive: &'a [u8],
    hash: &'a str,
    target: &'a str,
    version: &'a str,
    library: &'a str,
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

pub(crate) fn absolute_root(path: &Path) -> Result<PathBuf, Error> {
    if path.as_os_str().is_empty() {
        return Err(Error::InvalidInput(
            "seekdb cache directory must not be empty".into(),
        ));
    }
    if !path.is_absolute() {
        return Err(Error::InvalidInput(format!(
            "seekdb cache directory must be an absolute path: {}",
            path.display()
        )));
    }
    if path.as_os_str().as_bytes().contains(&0) {
        return Err(Error::InvalidInput(
            "seekdb cache directory contains NUL".into(),
        ));
    }
    Ok(path.to_path_buf())
}

fn select_root(
    explicit: Option<&Path>,
    environment: Option<OsString>,
    default: Option<PathBuf>,
) -> Result<PathBuf, Error> {
    if let Some(path) = explicit {
        return absolute_root(path);
    }
    if let Some(path) = environment {
        return absolute_root(Path::new(&path));
    }
    absolute_root(&default.ok_or_else(|| Error::Setup(
        "cannot determine the user cache directory; set SEEKDB_CACHE_DIR or call set_runtime_cache_dir".into()
    ))?)
}

impl Bundle<'static> {
    pub(crate) fn embedded(explicit: Option<&Path>) -> Result<Self, Error> {
        let default = dirs::cache_dir().map(|path| {
            let path = path.join("seekdb");
            // On Windows, cache_dir resolves LocalAppData, which also contains
            // non-cache application data. Windows native bundles are not yet supported.
            let path = if cfg!(windows) {
                path.join("cache")
            } else {
                path
            };
            path.join("native")
        });
        let root = select_root(explicit, std::env::var_os("SEEKDB_CACHE_DIR"), default)?;
        Ok(Self {
            root,
            archive: embedded::ARCHIVE,
            hash: embedded::SHA256,
            target: embedded::TARGET,
            version: embedded::VERSION,
            library: embedded::LIBRARY,
        })
    }
}

fn cache_error(path: &Path, operation: &'static str, source: io::Error) -> Error {
    Error::RuntimeCache {
        path: path.to_path_buf(),
        operation,
        source,
    }
}

fn private_directory(path: &Path) -> Result<(), Error> {
    fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(path)
        .map_err(|error| cache_error(path, "create cache directory", error))?;
    let metadata =
        fs::metadata(path).map_err(|error| cache_error(path, "inspect cache directory", error))?;
    // SAFETY: geteuid has no preconditions and does not change process state.
    if metadata.uid() != unsafe { libc::geteuid() } || metadata.mode() & 0o022 != 0 {
        return Err(Error::Setup(format!(
            "seekdb cache directory {} must be owned by the current user and not writable by other users; select another directory with SEEKDB_CACHE_DIR or set_runtime_cache_dir",
            path.display()
        )));
    }
    Ok(())
}

fn check_executable_mount(path: &Path) -> Result<(), Error> {
    #[cfg(not(target_os = "linux"))]
    let _ = path;
    #[cfg(target_os = "linux")]
    {
        let c_path = std::ffi::CString::new(path.as_os_str().as_bytes())
            .map_err(|_| Error::InvalidInput("seekdb cache directory contains NUL".into()))?;
        let mut information = std::mem::MaybeUninit::<libc::statvfs>::uninit();
        // SAFETY: c_path is NUL-terminated and information points to writable storage.
        if unsafe { libc::statvfs(c_path.as_ptr(), information.as_mut_ptr()) } != 0 {
            return Err(cache_error(
                path,
                "inspect cache mount",
                io::Error::last_os_error(),
            ));
        }
        // SAFETY: statvfs succeeded and initialized the structure.
        if unsafe { information.assume_init() }.f_flag & libc::ST_NOEXEC != 0 {
            return Err(Error::RuntimeCacheNoExec {
                path: path.to_path_buf(),
            });
        }
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

impl Bundle<'_> {
    fn expanded(&self) -> PathBuf {
        self.root
            .join(self.target)
            .join(self.version)
            .join(self.hash)
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

    /// Check on every open, including after Native's function pointers are cached.
    pub(crate) fn ensure(&self) -> Result<PathBuf, Error> {
        private_directory(&self.root)?;
        check_executable_mount(&self.root)?;
        let expanded = self.expanded();
        let parent = expanded.parent().unwrap();
        private_directory(parent)?;
        let lock_path = parent.join(format!(".{}.lock", self.hash));
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(&lock_path)
            .map_err(|error| cache_error(&lock_path, "open cache lock", error))?;
        lock.lock_exclusive()
            .map_err(|error| cache_error(&lock_path, "lock cache", error))?;
        // The lock lives outside the replaceable entry and unlocks on file drop.
        if let Ok(path) = self.valid_cache(&expanded) {
            return Ok(path);
        }
        if format!("{:x}", Sha256::digest(self.archive)) != self.hash {
            return Err(Error::Setup(
                "embedded seekdb archive SHA-256 mismatch".into(),
            ));
        }
        let staging = tempfile::Builder::new()
            .prefix(".extract-")
            .tempdir_in(parent)
            .map_err(|error| cache_error(parent, "create staging directory", error))?;
        tar::Archive::new(GzDecoder::new(self.archive))
            .unpack(staging.path())
            .map_err(|error| cache_error(staging.path(), "extract embedded archive", error))?;
        let native = native_directory(staging.path(), self.library)
            .map_err(|error| cache_error(staging.path(), "validate native layout", error))?;
        let mut entries = Vec::new();
        // Canonical paths also validate that archive symlinks stay within the bundle.
        let canonical_staging = staging
            .path()
            .canonicalize()
            .map_err(|error| cache_error(staging.path(), "resolve staging directory", error))?;
        inventory(&canonical_staging, &canonical_staging, &mut entries)
            .map_err(|error| cache_error(staging.path(), "inventory native bundle", error))?;
        let manifest = Manifest {
            hash: self.hash.into(),
            native_dir: native.strip_prefix(staging.path()).unwrap().into(),
            entries,
        };
        let marker = staging.path().join(".complete");
        let bytes =
            serde_json::to_vec(&manifest).map_err(|error| Error::Setup(error.to_string()))?;
        fs::write(&marker, bytes)
            .map_err(|error| cache_error(&marker, "write completion manifest", error))?;
        if fs::symlink_metadata(&expanded).is_ok() {
            // Never truncate or overwrite files mapped by another process. Keep
            // incomplete entries aside: their files may still be in active use.
            let retired = tempfile::Builder::new()
                .prefix(".retired-")
                .tempdir_in(parent)
                .map_err(|error| cache_error(parent, "create retired entry directory", error))?;
            fs::rename(&expanded, retired.path().join("bundle"))
                .map_err(|error| cache_error(&expanded, "retire incomplete cache", error))?;
            let _ = retired.keep();
        }
        fs::rename(staging.path(), &expanded)
            .map_err(|error| cache_error(&expanded, "publish native cache", error))?;
        self.valid_cache(&expanded)
            .map_err(|error| cache_error(&expanded, "validate native cache", error))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::{write::GzEncoder, Compression};
    use std::io::Write;

    fn archive() -> Vec<u8> {
        let mut archive = tar::Builder::new(GzEncoder::new(Vec::new(), Compression::fast()));
        for (name, bytes, mode) in [
            ("package/lib/libseekdb.so", "native", 0o644),
            ("package/lib/seekdb", "#!/bin/sh\nexit 0\n", 0o755),
            ("package/lib/.libs/libaio.so.1", "dependency", 0o644),
        ] {
            let mut header = tar::Header::new_gnu();
            header.set_size(bytes.len() as u64);
            header.set_mode(mode);
            header.set_cksum();
            archive
                .append_data(&mut header, name, bytes.as_bytes())
                .unwrap();
        }
        let mut header = tar::Header::new_gnu();
        header.set_entry_type(tar::EntryType::Symlink);
        header.set_size(0);
        header.set_mode(0o777);
        header.set_link_name(".libs/libaio.so.1").unwrap();
        header.set_cksum();
        archive
            .append_data(&mut header, "package/lib/libaio.so.1", io::empty())
            .unwrap();
        archive.into_inner().unwrap().finish().unwrap()
    }

    #[test]
    fn root_precedence_and_invalid_override() {
        let default = Some(PathBuf::from("/default"));
        assert_eq!(
            select_root(
                Some(Path::new("/api")),
                Some("/env".into()),
                default.clone()
            )
            .unwrap(),
            Path::new("/api")
        );
        assert_eq!(
            select_root(None, Some("/env".into()), default.clone()).unwrap(),
            Path::new("/env")
        );
        assert_eq!(
            select_root(None, None, default).unwrap(),
            Path::new("/default")
        );
        assert!(select_root(None, Some("relative".into()), Some("/default".into())).is_err());
        assert!(select_root(None, Some("".into()), Some("/default".into())).is_err());
    }

    #[test]
    fn concurrent_extract_reuse_and_repair_missing_dependency() {
        let root = tempfile::tempdir().unwrap();
        let bytes = archive();
        let hash = format!("{:x}", Sha256::digest(&bytes));
        let bundle = Bundle {
            root: root.path().join("cache"),
            archive: &bytes,
            hash: &hash,
            target: "test-target",
            version: "1",
            library: "libseekdb.so",
        };
        std::thread::scope(|scope| {
            let threads: Vec<_> = (0..4)
                .map(|_| scope.spawn(|| bundle.ensure().unwrap()))
                .collect();
            for thread in threads {
                assert!(thread.join().unwrap().is_file());
            }
        });
        let path = bundle.ensure().unwrap();
        let original = fs::metadata(&path).unwrap().ino();
        assert_eq!(
            fs::metadata(bundle.ensure().unwrap()).unwrap().ino(),
            original
        );
        let native = path.parent().unwrap();
        assert!(fs::symlink_metadata(native.join("libaio.so.1"))
            .unwrap()
            .file_type()
            .is_symlink());
        assert_ne!(
            fs::metadata(native.join("seekdb")).unwrap().mode() & 0o111,
            0
        );
        fs::remove_file(native.join(".libs/libaio.so.1")).unwrap();
        bundle.ensure().unwrap();
        assert_eq!(fs::read(native.join("libaio.so.1")).unwrap(), b"dependency");
        assert_ne!(fs::metadata(&path).unwrap().ino(), original);
        fs::remove_dir_all(bundle.expanded()).unwrap();
        assert!(bundle.ensure().unwrap().is_file());
    }

    #[test]
    fn rejects_corrupt_archive_without_marking_complete() {
        let root = tempfile::tempdir().unwrap();
        let bundle = Bundle {
            root: root.path().join("cache"),
            archive: b"corrupt",
            hash: "wrong-hash",
            target: "target",
            version: "1",
            library: "libseekdb.so",
        };
        assert!(bundle.ensure().unwrap_err().to_string().contains("SHA-256"));
        assert!(!bundle.expanded().exists());
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn diagnoses_noexec_before_extracting() {
        let Ok(root) = tempfile::tempdir_in("/dev/shm") else {
            return;
        };
        match check_executable_mount(root.path()) {
            Err(Error::RuntimeCacheNoExec { path }) => assert_eq!(path, root.path()),
            Ok(()) => return, // /dev/shm need not be mounted noexec on every host.
            other => panic!("unexpected mount diagnostic: {other:?}"),
        }
        let bundle = Bundle {
            root: root.path().join("cache"),
            archive: b"not needed",
            hash: "unused",
            target: "target",
            version: "1",
            library: "libseekdb.so",
        };
        let error = bundle.ensure().unwrap_err();
        assert!(matches!(error, Error::RuntimeCacheNoExec { .. }));
        assert!(error.to_string().contains("SEEKDB_CACHE_DIR"));
        assert!(!bundle.expanded().exists());
    }

    #[test]
    fn filesystem_error_keeps_path_operation_and_source() {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("file");
        File::create(&path)
            .unwrap()
            .write_all(b"not a directory")
            .unwrap();
        let bundle = Bundle {
            root: path.clone(),
            archive: b"unused",
            hash: "unused",
            target: "target",
            version: "1",
            library: "libseekdb.so",
        };
        let error = bundle.ensure().unwrap_err();
        assert!(
            matches!(&error, Error::RuntimeCache { path: actual, operation: "create cache directory", .. } if actual == &path)
        );
        assert!(std::error::Error::source(&error).is_some());
    }
}
