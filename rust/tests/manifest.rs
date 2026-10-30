#[allow(dead_code)]
#[path = "../bundle_manifest.rs"]
mod bundle_manifest;
#[allow(dead_code)]
#[path = "../manifest_cache.rs"]
mod manifest_cache;
#[allow(dead_code)]
#[path = "../native_cache.rs"]
mod native_cache;

use bundle_manifest::{Config, DownloadBase, PACKAGE_TOML};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::Read;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};

fn release() -> (Config, PathBuf, Vec<u8>) {
    let config = Config::from_package(PACKAGE_TOML).unwrap();
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("bundles")
        .join(config.manifest_name());
    let bytes = fs::read(&path).unwrap();
    config.verify_manifest(&bytes).unwrap();
    (config, path, bytes)
}

#[test]
fn selects_exact_target_abi_and_applies_the_mirror_to_both_downloads() {
    let (config, _, bytes) = release();
    let base = DownloadBase::new(Some("https://mirror.example/releases")).unwrap();
    assert_eq!(
        base.file_url(&config.manifest_name()),
        format!(
            "https://mirror.example/releases/manifest-{}.json",
            config.native_version
        )
    );
    for (target, platform, library) in [
        (
            "x86_64-unknown-linux-gnu",
            "linux-x86_64-glibc2.28",
            "libseekdb.so",
        ),
        (
            "aarch64-unknown-linux-gnu",
            "linux-aarch64-glibc2.28",
            "libseekdb.so",
        ),
        ("x86_64-apple-darwin", "macos15.0-x86_64", "libseekdb.dylib"),
        ("aarch64-apple-darwin", "macos15.0-arm64", "libseekdb.dylib"),
        ("x86_64-pc-windows-msvc", "windows-x86_64", "seekdb.dll"),
    ] {
        let asset = config.asset(&bytes, target, &base).unwrap();
        assert!(asset.name.contains(platform));
        assert_eq!(asset.library, library);
        assert_eq!(asset.version, config.native_version);
        assert_eq!(
            asset.url,
            format!("https://mirror.example/releases/{}", asset.name)
        );
        assert_eq!(asset.sha256.len(), 64);
        assert!(asset.size_bytes > 0);
    }
    for target in [
        "x86_64-unknown-linux-musl",
        "x86_64-pc-windows-gnu",
        "aarch64-pc-windows-msvc",
    ] {
        assert!(config.asset(&bytes, target, &base).is_err());
    }
}

#[test]
fn rejects_invalid_prefixes_and_normalizes_directory_urls() {
    for value in [
        "",
        "relative",
        "http://mirror.example/",
        "https://user:secret@mirror.example/",
        "https://mirror.example/?token=secret",
        "https://mirror.example/#fragment",
    ] {
        assert!(DownloadBase::new(Some(value)).is_err(), "{value}");
    }
    assert_eq!(
        DownloadBase::new(Some("https://mirror.example/path%20with%20spaces"))
            .unwrap()
            .file_url("manifest.json"),
        "https://mirror.example/path%20with%20spaces/manifest.json"
    );
    assert!(DownloadBase::new(None)
        .unwrap()
        .file_url("manifest.json")
        .starts_with("https://obbusiness-private.oss-cn-shanghai.aliyuncs.com/"));
}

#[test]
fn upgrade_uses_only_metadata_and_the_matching_manifest() {
    // Synthetic future-release metadata, not evidence of an existing asset.
    let version = "9.1.0.0-r7";
    let bytes = serde_json::to_vec(&serde_json::json!({
        "schema_version": "1.0", "package": "libseekdb", "package_version": version,
        "artifacts": [{"os": "linux", "architecture": "aarch64", "runtime": {
            "file": format!("libseekdb-{version}-linux-aarch64.tar.gz"),
            "sha256": "1".repeat(64), "size_bytes": 123
        }}]
    }))
    .unwrap();
    let metadata = format!(
        "[package.metadata.seekdb]\nnative-version = '{version}'\nmanifest-sha256 = '{:x}'\n",
        Sha256::digest(&bytes)
    );
    let config = Config::from_package(&metadata).unwrap();
    let asset = config
        .asset(
            &bytes,
            "aarch64-unknown-linux-gnu",
            &DownloadBase::new(None).unwrap(),
        )
        .unwrap();
    assert_eq!(asset.version, version);
    assert_eq!(config.manifest_name(), format!("manifest-{version}.json"));
    for bad in ["../version", "", "/absolute", "1/../../other"] {
        assert!(Config::from_package(&metadata.replace(version, bad)).is_err());
    }
    assert!(Config::from_package(
        &metadata.replace(&format!("{:x}", Sha256::digest(&bytes)), "invalid")
    )
    .is_err());
}

#[test]
fn rejects_changed_identity_unsafe_names_ambiguous_assets_and_oversized_metadata() {
    let (config, _, bytes) = release();
    let base = DownloadBase::new(None).unwrap();
    for mutation in ["identity", "name", "duplicate", "hash", "json"] {
        let mut json: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        match mutation {
            "identity" => json["package_version"] = "other-version".into(),
            "name" => json["artifacts"][0]["runtime"]["file"] = "../bundle.tar.gz".into(),
            "duplicate" => {
                let duplicate = json["artifacts"][0].clone();
                json["artifacts"].as_array_mut().unwrap().push(duplicate);
            }
            "hash" => json["artifacts"][0]["runtime"]["sha256"] = "invalid".into(),
            "json" => json = serde_json::Value::Null,
            _ => unreachable!(),
        }
        let changed = serde_json::to_vec(&json).unwrap();
        let mut fixture_config = config.clone();
        fixture_config.manifest_sha256 = format!("{:x}", Sha256::digest(&changed));
        assert!(
            fixture_config
                .asset(&changed, "x86_64-unknown-linux-gnu", &base)
                .is_err(),
            "{mutation}"
        );
    }
    assert!(config
        .verify_manifest(&vec![0; bundle_manifest::MAX_MANIFEST_BYTES as usize + 1])
        .is_err());
}

#[test]
fn cold_download_is_verified_and_reused_by_another_build_without_network() {
    let temporary = tempfile::tempdir().unwrap();
    let (config, bundled, bytes) = release();
    let root = temporary.path().join("cache");
    let out = temporary.path().join("out");
    fs::create_dir(&out).unwrap();
    let downloads = AtomicUsize::new(0);
    assert_eq!(
        manifest_cache::prepare_manifest(&out, Some(&root), &config, &bundled, || {
            downloads.fetch_add(1, Ordering::SeqCst);
            Ok(bytes.clone())
        })
        .unwrap(),
        bytes
    );
    assert_eq!(downloads.load(Ordering::SeqCst), 1);
    let shared = manifest_cache::cache_path(&root, &config);
    assert_eq!(
        shared,
        root.join("manifests")
            .join(&config.native_version)
            .join(&config.manifest_sha256)
            .join("manifest.json")
    );
    let second = temporary.path().join("second");
    fs::create_dir(&second).unwrap();
    assert_eq!(
        manifest_cache::prepare_manifest(&second, Some(&root), &config, &bundled, || panic!(
            "must reuse verified shared manifest"
        ))
        .unwrap(),
        bytes
    );
    fs::remove_dir_all(&root).unwrap();
    manifest_cache::prepare_manifest(&out, Some(&root), &config, &bundled, || {
        panic!("OUT_DIR repairs the cleaned metadata cache offline")
    })
    .unwrap();
    assert_eq!(fs::read(shared).unwrap(), bytes);
}

#[test]
fn corrupt_cache_is_atomically_repaired_from_the_verified_build_copy() {
    let temporary = tempfile::tempdir().unwrap();
    let (config, bundled, bytes) = release();
    let root = temporary.path().join("cache");
    manifest_cache::prepare_manifest(temporary.path(), Some(&root), &config, &bundled, || {
        Ok(bytes.clone())
    })
    .unwrap();
    let shared = manifest_cache::cache_path(&root, &config);
    fs::write(&shared, b"corrupt cache").unwrap();
    let mut previous_reader = fs::File::open(&shared).unwrap();
    manifest_cache::prepare_manifest(temporary.path(), Some(&root), &config, &bundled, || {
        panic!("must reuse verified build copy")
    })
    .unwrap();
    assert_eq!(fs::read(shared).unwrap(), bytes);
    let mut old_bytes = Vec::new();
    previous_reader.read_to_end(&mut old_bytes).unwrap();
    assert_eq!(old_bytes, b"corrupt cache");
}

#[test]
fn transport_failure_uses_the_packaged_snapshot_but_bad_remote_content_is_fatal() {
    let temporary = tempfile::tempdir().unwrap();
    let (config, bundled, bytes) = release();
    let offline = temporary.path().join("offline");
    fs::create_dir(&offline).unwrap();
    assert_eq!(
        manifest_cache::prepare_manifest(&offline, None, &config, &bundled, || Err(
            "network unavailable".into()
        ))
        .unwrap(),
        bytes
    );
    let invalid = temporary.path().join("invalid");
    fs::create_dir(&invalid).unwrap();
    let error = manifest_cache::prepare_manifest(&invalid, None, &config, &bundled, || {
        Ok(b"bad mirror response".to_vec())
    })
    .unwrap_err();
    assert!(error.contains("SHA-256 mismatch"), "{error}");
    assert!(!invalid.join(config.manifest_name()).exists());
    let missing = temporary.path().join("missing");
    let error = manifest_cache::prepare_manifest(&invalid, None, &config, &missing, || {
        Err("network unavailable".into())
    })
    .unwrap_err();
    assert!(error.contains("no verified packaged manifest"));
}

#[test]
fn unavailable_metadata_cache_keeps_an_independent_verified_build_copy() {
    let temporary = tempfile::tempdir().unwrap();
    let (config, bundled, bytes) = release();
    let unusable = temporary.path().join("file");
    fs::write(&unusable, b"not a directory").unwrap();
    manifest_cache::prepare_manifest(temporary.path(), Some(&unusable), &config, &bundled, || {
        Ok(bytes.clone())
    })
    .unwrap();
    assert_eq!(
        fs::read(temporary.path().join(config.manifest_name())).unwrap(),
        bytes
    );
}

#[test]
fn concurrent_builds_publish_complete_manifests_without_locks() {
    let temporary = tempfile::tempdir().unwrap();
    let (config, bundled, bytes) = release();
    let root = temporary.path().join("cache");
    std::thread::scope(|scope| {
        let children: Vec<_> = (0..8)
            .map(|index| {
                let out = temporary.path().join(format!("build-{index}"));
                fs::create_dir(&out).unwrap();
                let (config, bundled, bytes, root) = (&config, &bundled, &bytes, &root);
                scope.spawn(move || {
                    manifest_cache::prepare_manifest(&out, Some(root), config, bundled, || {
                        Ok(bytes.clone())
                    })
                    .unwrap()
                })
            })
            .collect();
        for child in children {
            assert_eq!(child.join().unwrap(), bytes);
        }
    });
    let shared = manifest_cache::cache_path(&root, &config);
    assert_eq!(fs::read(&shared).unwrap(), bytes);
    assert_eq!(fs::read_dir(shared.parent().unwrap()).unwrap().count(), 1);
}

#[cfg(windows)]
#[test]
fn foreign_windows_readers_preserve_build_inputs_and_can_defer_shared_repair() {
    use std::os::windows::fs::OpenOptionsExt;
    let temporary = tempfile::tempdir().unwrap();
    let (config, bundled, bytes) = release();
    let root = temporary.path().join("cache");
    manifest_cache::prepare_manifest(temporary.path(), Some(&root), &config, &bundled, || {
        Ok(bytes.clone())
    })
    .unwrap();
    let shared = manifest_cache::cache_path(&root, &config);
    fs::write(&shared, b"corrupt cache").unwrap();
    // Emulate a foreign reader that does not grant FILE_SHARE_DELETE.
    // The SDK never takes a cache file lock of its own.
    let mut reader = fs::OpenOptions::new()
        .read(true)
        .share_mode(1 | 2)
        .open(&shared)
        .unwrap();
    assert_eq!(
        manifest_cache::prepare_manifest(
            temporary.path(),
            Some(&root),
            &config,
            &bundled,
            || panic!("must reuse verified build copy")
        )
        .unwrap(),
        bytes
    );
    let published = fs::read(&shared).unwrap();
    assert!(
        published == bytes || published == b"corrupt cache",
        "the shared name must expose a complete old or new file"
    );
    let mut old_bytes = Vec::new();
    reader.read_to_end(&mut old_bytes).unwrap();
    assert_eq!(
        old_bytes, b"corrupt cache",
        "never overwrite a reader's file in place"
    );
    assert_eq!(
        fs::read(temporary.path().join(config.manifest_name())).unwrap(),
        bytes
    );
    drop(reader);
    manifest_cache::prepare_manifest(temporary.path(), Some(&root), &config, &bundled, || {
        panic!("must repair without network")
    })
    .unwrap();
    assert_eq!(fs::read(shared).unwrap(), bytes);
}
