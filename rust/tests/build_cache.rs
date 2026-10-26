#[path = "../build_cache.rs"]
mod cache;
#[allow(dead_code)]
#[path = "../native_cache.rs"]
mod native_cache;

use flate2::{write::GzEncoder, Compression};
use native_cache::NativeCache;
use sha2::{Digest, Sha256};
use std::fs;
use std::os::unix::fs::MetadataExt;
use std::sync::atomic::{AtomicUsize, Ordering};

const NAME: &str = "native.tar.gz";

fn archive() -> Vec<u8> {
    let mut archive = tar::Builder::new(GzEncoder::new(Vec::new(), Compression::fast()));
    for (path, bytes, mode) in [
        ("package/lib/libseekdb.so", "native", 0o644),
        ("package/lib/seekdb", "server", 0o755),
    ] {
        let mut header = tar::Header::new_gnu();
        header.set_size(bytes.len() as u64);
        header.set_mode(mode);
        header.set_cksum();
        archive
            .append_data(&mut header, path, bytes.as_bytes())
            .unwrap();
    }
    archive.into_inner().unwrap().finish().unwrap()
}

fn native_cache<'a>(root: std::path::PathBuf, hash: &'a str) -> NativeCache<'a> {
    NativeCache {
        root,
        hash,
        target: "test-target",
        version: "1",
        library: "libseekdb.so",
    }
}

#[test]
fn builds_and_runtime_share_one_prepared_entry() {
    let temporary = tempfile::tempdir().unwrap();
    let bytes = archive();
    let hash = format!("{:x}", Sha256::digest(&bytes));
    let native = native_cache(temporary.path().join("native"), &hash);
    let downloads = AtomicUsize::new(0);
    std::thread::scope(|scope| {
        let threads: Vec<_> = (0..4)
            .map(|index| {
                let native = &native;
                let hash = &hash;
                let downloads = &downloads;
                let bytes = &bytes;
                let out = temporary.path().join(format!("build-{index}"));
                scope.spawn(move || {
                    fs::create_dir(&out).unwrap();
                    cache::prepare_archive(&out, Some(native), NAME, hash, |_, destination| {
                        downloads.fetch_add(1, Ordering::SeqCst);
                        fs::write(destination, bytes).map_err(|error| error.to_string())
                    })
                    .unwrap()
                })
            })
            .collect();
        for thread in threads {
            assert_eq!(fs::read(thread.join().unwrap()).unwrap(), bytes);
        }
    });
    assert!((1..=4).contains(&downloads.load(Ordering::SeqCst)));
    assert_eq!(fs::read(native.archive_path()).unwrap(), bytes);
    assert!(native.entry().join(".complete").is_file());
    let library = native.entry().join("package/lib/libseekdb.so");
    let inode = fs::metadata(&library).unwrap().ino();
    // This is the same cache operation used by Bundle::ensure at runtime.
    assert_eq!(native.ensure(&bytes).unwrap(), library);
    assert_eq!(fs::metadata(&library).unwrap().ino(), inode);
    fs::remove_dir_all(&native.root).unwrap();
    assert_eq!(
        fs::read(temporary.path().join("build-0").join(NAME)).unwrap(),
        bytes
    );
}

#[test]
fn archive_repair_preserves_the_prepared_native_files() {
    let temporary = tempfile::tempdir().unwrap();
    let out = temporary.path().join("build");
    fs::create_dir(&out).unwrap();
    let bytes = archive();
    fs::write(out.join(NAME), &bytes).unwrap();
    let hash = format!("{:x}", Sha256::digest(&bytes));
    let native = native_cache(temporary.path().join("native"), &hash);
    cache::prepare_archive(&out, Some(&native), NAME, &hash, |_, _| {
        panic!("must not download")
    })
    .unwrap();
    let library = native.entry().join("package/lib/libseekdb.so");
    let inode = fs::metadata(&library).unwrap().ino();
    fs::write(native.archive_path(), b"corrupt cached archive").unwrap();
    cache::prepare_archive(&out, Some(&native), NAME, &hash, |_, _| {
        panic!("must not download")
    })
    .unwrap();
    assert_eq!(fs::read(native.archive_path()).unwrap(), bytes);
    assert_eq!(fs::metadata(&library).unwrap().ino(), inode);
}

#[test]
fn wrong_download_is_rejected_and_a_correct_archive_prepares_the_runtime() {
    let temporary = tempfile::tempdir().unwrap();
    let out = temporary.path().join("build");
    fs::create_dir(&out).unwrap();
    let bytes = archive();
    let hash = format!("{:x}", Sha256::digest(&bytes));
    let native = native_cache(temporary.path().join("native"), &hash);
    assert!(
        cache::prepare_archive(&out, Some(&native), NAME, &hash, |_, destination| {
            fs::write(destination, b"wrong response").map_err(|error| error.to_string())
        })
        .unwrap_err()
        .contains("SHA-256")
    );
    assert!(!native.entry().join(".complete").exists());
    cache::prepare_archive(&out, Some(&native), NAME, &hash, |_, destination| {
        fs::write(destination, &bytes).map_err(|error| error.to_string())
    })
    .unwrap();
    assert!(native.entry().join(".complete").is_file());
}

#[test]
fn unavailable_user_cache_falls_back_to_cargo_output() {
    let out = tempfile::tempdir().unwrap();
    let unavailable = out.path().join("file");
    fs::write(&unavailable, b"not a directory").unwrap();
    let bytes = archive();
    let hash = format!("{:x}", Sha256::digest(&bytes));
    let native = native_cache(unavailable, &hash);
    let package =
        cache::prepare_archive(out.path(), Some(&native), NAME, &hash, |_, destination| {
            fs::write(destination, &bytes).map_err(|error| error.to_string())
        })
        .unwrap();
    assert_eq!(fs::read(package).unwrap(), bytes);
}
