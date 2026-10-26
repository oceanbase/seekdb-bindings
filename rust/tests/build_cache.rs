#[path = "../build_cache.rs"]
mod cache;

use sha2::{Digest, Sha256};
use std::fs;
use std::sync::atomic::{AtomicUsize, Ordering};

const ARCHIVE: &[u8] = b"verified native archive fixture";
const NAME: &str = "native.tar.gz";

#[test]
fn concurrent_builds_download_once_and_keep_independent_compiler_inputs() {
    let temporary = tempfile::tempdir().unwrap();
    let shared = temporary.path().join("downloads");
    let expected = format!("{:x}", Sha256::digest(ARCHIVE));
    let downloads = AtomicUsize::new(0);
    std::thread::scope(|scope| {
        let threads: Vec<_> = (0..4)
            .map(|index| {
                let shared = &shared;
                let expected = &expected;
                let downloads = &downloads;
                let out = temporary.path().join(format!("build-{index}"));
                scope.spawn(move || {
                    fs::create_dir(&out).unwrap();
                    cache::prepare_archive(&out, Some(shared), NAME, expected, |_, destination| {
                        downloads.fetch_add(1, Ordering::SeqCst);
                        fs::write(destination, ARCHIVE).map_err(|error| error.to_string())
                    })
                    .unwrap()
                })
            })
            .collect();
        for thread in threads {
            assert_eq!(fs::read(thread.join().unwrap()).unwrap(), ARCHIVE);
        }
    });
    assert_eq!(downloads.load(Ordering::SeqCst), 1);
    fs::remove_dir_all(&shared).unwrap();
    assert_eq!(
        fs::read(temporary.path().join("build-0").join(NAME)).unwrap(),
        ARCHIVE
    );
}

#[test]
fn valid_build_output_seeds_or_repairs_shared_cache_without_a_download() {
    let temporary = tempfile::tempdir().unwrap();
    let out = temporary.path().join("build");
    let shared = temporary.path().join("downloads");
    fs::create_dir(&out).unwrap();
    fs::write(out.join(NAME), ARCHIVE).unwrap();
    let expected = format!("{:x}", Sha256::digest(ARCHIVE));
    for _ in 0..2 {
        cache::prepare_archive(&out, Some(&shared), NAME, &expected, |_, _| {
            panic!("must not download")
        })
        .unwrap();
        assert_eq!(fs::read(shared.join(NAME)).unwrap(), ARCHIVE);
        fs::write(shared.join(NAME), b"corrupt cached archive").unwrap();
    }
}

#[test]
fn corrupt_shared_archive_is_replaced_and_wrong_download_is_rejected() {
    let temporary = tempfile::tempdir().unwrap();
    let shared = temporary.path().join("downloads");
    let out = temporary.path().join("build");
    fs::create_dir(&shared).unwrap();
    fs::create_dir(&out).unwrap();
    fs::write(shared.join(NAME), b"corrupt").unwrap();
    let expected = format!("{:x}", Sha256::digest(ARCHIVE));
    assert!(
        cache::prepare_archive(&out, Some(&shared), NAME, &expected, |_, destination| {
            fs::write(destination, b"wrong response").map_err(|error| error.to_string())
        })
        .unwrap_err()
        .contains("SHA-256")
    );
    assert!(!out.join(NAME).exists());
    let package = cache::prepare_archive(&out, Some(&shared), NAME, &expected, |_, destination| {
        fs::write(destination, ARCHIVE).map_err(|error| error.to_string())
    })
    .unwrap();
    assert_eq!(fs::read(package).unwrap(), ARCHIVE);
}

#[test]
fn unavailable_user_cache_falls_back_to_cargo_output() {
    let out = tempfile::tempdir().unwrap();
    let unavailable = out.path().join("file");
    fs::write(&unavailable, b"not a directory").unwrap();
    let expected = format!("{:x}", Sha256::digest(ARCHIVE));
    let package = cache::prepare_archive(
        out.path(),
        Some(&unavailable),
        NAME,
        &expected,
        |_, destination| fs::write(destination, ARCHIVE).map_err(|error| error.to_string()),
    )
    .unwrap();
    assert_eq!(package, out.path().join(NAME));
    assert_eq!(fs::read(package).unwrap(), ARCHIVE);
}
