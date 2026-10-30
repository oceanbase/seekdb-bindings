#[allow(dead_code)]
#[path = "../build_cache.rs"]
mod build_cache;
#[allow(dead_code)]
#[path = "../native_cache.rs"]
mod native_cache;

use flate2::{write::GzEncoder, Compression};
use native_cache::NativeCache;
use sha2::{Digest, Sha256};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use std::time::{Duration, Instant};

fn archive() -> Vec<u8> {
    let mut builder = tar::Builder::new(GzEncoder::new(Vec::new(), Compression::fast()));
    for (path, contents, mode) in [
        ("package/lib/libseekdb.so", "native", 0o644),
        ("package/lib/seekdb", "server", 0o755),
    ] {
        let mut header = tar::Header::new_gnu();
        header.set_size(contents.len() as u64);
        header.set_mode(mode);
        header.set_cksum();
        builder
            .append_data(&mut header, path, contents.as_bytes())
            .unwrap();
    }
    builder.into_inner().unwrap().finish().unwrap()
}

fn cache<'a>(root: &Path, hash: &'a str) -> NativeCache<'a> {
    NativeCache {
        root: root.join("native"),
        target: "target",
        version: "1",
        hash,
        library: "libseekdb.so",
    }
}

fn wait_for(path: &Path) {
    let deadline = Instant::now() + Duration::from_secs(15);
    while !path.exists() {
        assert!(
            Instant::now() < deadline,
            "timed out waiting for {}",
            path.display()
        );
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn cache_worker() {
    let Some(root) = std::env::var_os("SEEKDB_TEST_RENAME_ROOT") else {
        return;
    };
    let root = PathBuf::from(root);
    let index: usize = std::env::var("SEEKDB_TEST_RENAME_WORKER")
        .unwrap()
        .parse()
        .unwrap();
    let bytes = archive();
    let hash = format!("{:x}", Sha256::digest(&bytes));
    let native = cache(&root, &hash);
    fs::write(root.join(format!("ready-{index}")), []).unwrap();
    wait_for(&root.join("go"));
    if index & 1 == 0 {
        let out = root.join(format!("build-{index}"));
        fs::create_dir(&out).unwrap();
        let input = build_cache::prepare_archive(
            &out,
            Some(&native),
            "input.tar.gz",
            &hash,
            |_, destination| fs::write(destination, &bytes).map_err(|error| error.to_string()),
        )
        .unwrap();
        assert_eq!(fs::read(input).unwrap(), bytes);
    }
    let library = native.ensure(&bytes).unwrap();
    assert_eq!(fs::read(library).unwrap(), b"native");
}

fn competing_processes(repair: bool) {
    let temporary = tempfile::tempdir().unwrap();
    let bytes = archive();
    let hash = format!("{:x}", Sha256::digest(&bytes));
    let native = cache(temporary.path(), &hash);
    if repair {
        native.ensure(&bytes).unwrap();
        fs::remove_file(native.entry().join("package/lib/seekdb")).unwrap();
    }
    let mut children = Vec::new();
    for index in 0..6 {
        children.push(
            Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "cache_worker", "--nocapture"])
                .env("SEEKDB_TEST_RENAME_ROOT", temporary.path())
                .env("SEEKDB_TEST_RENAME_WORKER", index.to_string())
                .stdout(Stdio::piped())
                .stderr(Stdio::piped())
                .spawn()
                .unwrap(),
        );
    }
    for index in 0..6 {
        wait_for(&temporary.path().join(format!("ready-{index}")));
    }
    let stop = Arc::new(AtomicBool::new(false));
    let observer_stop = stop.clone();
    let entry = native.entry();
    let observed_hash = hash.clone();
    let observer = std::thread::spawn(move || {
        let mut seen = repair;
        while !observer_stop.load(Ordering::SeqCst) {
            match fs::read(entry.join("package/lib/libseekdb.so")) {
                Ok(contents) => {
                    assert_eq!(contents, b"native");
                    seen = true;
                }
                Err(error) if error.kind() == std::io::ErrorKind::NotFound && !seen => {}
                Err(error) => {
                    panic!("published library disappeared during concurrent publication: {error}")
                }
            }
            if seen {
                let manifest: serde_json::Value =
                    serde_json::from_slice(&fs::read(entry.join(".complete")).unwrap()).unwrap();
                assert_eq!(manifest["hash"].as_str(), Some(observed_hash.as_str()));
                // On Windows repair changes this pointer to a retained complete
                // generation; readers must always be able to follow it.
                let native = manifest["native_dir"].as_str().unwrap();
                assert_eq!(
                    fs::read(entry.join(native).join("libseekdb.so")).unwrap(),
                    b"native"
                );
            }
            std::thread::yield_now();
        }
    });
    fs::write(temporary.path().join("go"), []).unwrap();
    let outputs: Vec<_> = children
        .into_iter()
        .map(|child| child.wait_with_output().unwrap())
        .collect();
    stop.store(true, Ordering::SeqCst);
    observer.join().unwrap();
    for output in outputs {
        assert!(
            output.status.success(),
            "worker failed:\n{}\n{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
    }
    assert_eq!(fs::read(native.ensure(&bytes).unwrap()).unwrap(), b"native");
    assert_eq!(fs::read(native.archive_path()).unwrap(), bytes);
    for entry in fs::read_dir(native.entry().parent().unwrap()).unwrap() {
        assert!(!entry
            .unwrap()
            .file_name()
            .to_string_lossy()
            .ends_with(".lock"));
    }
}

#[test]
fn independent_builds_and_runtimes_publish_one_complete_entry() {
    competing_processes(false);
}

#[test]
fn concurrent_repair_keeps_the_library_path_available() {
    competing_processes(true);
}

#[test]
fn abandoned_staging_directory_does_not_block_a_new_publisher() {
    let temporary = tempfile::tempdir().unwrap();
    let bytes = archive();
    let hash = format!("{:x}", Sha256::digest(&bytes));
    let native = cache(temporary.path(), &hash);
    native.prepare_directory().unwrap();
    let abandoned = native.entry().parent().unwrap().join(".extract-abandoned");
    fs::create_dir(&abandoned).unwrap();
    fs::write(abandoned.join("partial"), b"incomplete extraction").unwrap();
    assert_eq!(fs::read(native.ensure(&bytes).unwrap()).unwrap(), b"native");
}

#[cfg(windows)]
#[test]
fn repairs_retain_previous_generations_for_existing_readers() {
    let temporary = tempfile::tempdir().unwrap();
    let bytes = archive();
    let hash = format!("{:x}", Sha256::digest(&bytes));
    let native = cache(temporary.path(), &hash);
    let mut previous = vec![native.ensure(&bytes).unwrap()];
    for _ in 0..3 {
        fs::remove_file(previous.last().unwrap().with_file_name("seekdb")).unwrap();
        let repaired = native.ensure(&bytes).unwrap();
        assert!(repaired.with_file_name("seekdb").is_file());
        assert!(!previous.contains(&repaired));
        for old in &previous {
            assert_eq!(fs::read(old).unwrap(), b"native");
        }
        previous.push(repaired);
    }
    assert_eq!(fs::read(native.archive_path()).unwrap(), bytes);
}

#[test]
fn repair_preserves_open_completion_and_archive_readers() {
    use std::io::Read;
    let temporary = tempfile::tempdir().unwrap();
    let bytes = archive();
    let hash = format!("{:x}", Sha256::digest(&bytes));
    let native = cache(temporary.path(), &hash);
    let library = native.ensure(&bytes).unwrap();
    let marker = native.entry().join(".complete");
    let old_manifest = fs::read(&marker).unwrap();
    let mut manifest_reader = fs::File::open(&marker).unwrap();
    let mut archive_reader = fs::File::open(native.archive_path()).unwrap();
    fs::remove_file(library.with_file_name("seekdb")).unwrap();
    let repaired = native.ensure(&bytes).unwrap();
    assert_eq!(fs::read(&repaired).unwrap(), b"native");
    assert!(repaired.with_file_name("seekdb").is_file());
    assert_eq!(fs::read(native.archive_path()).unwrap(), bytes);
    let mut observed_manifest = Vec::new();
    manifest_reader.read_to_end(&mut observed_manifest).unwrap();
    assert_eq!(observed_manifest, old_manifest);
    let mut observed_archive = Vec::new();
    archive_reader.read_to_end(&mut observed_archive).unwrap();
    assert_eq!(observed_archive, bytes);
    let completed: serde_json::Value = serde_json::from_slice(&fs::read(marker).unwrap()).unwrap();
    assert_eq!(completed["hash"], hash);
    assert_eq!(
        native
            .entry()
            .join(completed["native_dir"].as_str().unwrap())
            .join("libseekdb.so"),
        repaired
    );
}
