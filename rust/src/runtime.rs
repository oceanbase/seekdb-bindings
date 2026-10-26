use crate::{native_cache, Error};
use std::ffi::OsString;
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};

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
        let default = native_cache::default_root();
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

impl Bundle<'_> {
    fn cache(&self) -> native_cache::NativeCache<'_> {
        native_cache::NativeCache {
            root: self.root.clone(),
            hash: self.hash,
            target: self.target,
            version: self.version,
            library: self.library,
        }
    }

    #[cfg(test)]
    fn expanded(&self) -> PathBuf {
        self.cache().entry()
    }

    pub(crate) fn ensure(&self) -> Result<PathBuf, Error> {
        let cache = self.cache();
        let _lock = cache.lock().map_err(map_cache_error)?;
        // Builds can prepare files on noexec mounts; only runtime use requires execution.
        check_executable_mount(&self.root)?;
        cache.ensure_locked(self.archive).map_err(map_cache_error)
    }
}

fn map_cache_error(error: native_cache::CacheError) -> Error {
    Error::RuntimeCache {
        path: error.path,
        operation: error.operation,
        source: error.source,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::{write::GzEncoder, Compression};
    use sha2::{Digest, Sha256};
    use std::fs::{self, File};
    use std::io::Write;
    use std::os::unix::fs::MetadataExt;

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
