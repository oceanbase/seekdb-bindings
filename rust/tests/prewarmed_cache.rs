use seekdb::Seekdb;
use std::fs;
use std::os::unix::fs::MetadataExt;
use std::path::PathBuf;

#[allow(dead_code)]
mod embedded {
    include!(concat!(env!("OUT_DIR"), "/native_bundle.rs"));
}

#[test]
fn runtime_reuses_the_build_prepared_library_without_reextracting() {
    // CI explicitly sets this for both cargo and its test processes. Builds
    // without a writable user cache can still use the OUT_DIR fallback.
    let Some(root) = std::env::var_os("SEEKDB_CACHE_DIR") else {
        return;
    };
    let entry = PathBuf::from(root)
        .join(embedded::TARGET)
        .join(embedded::VERSION)
        .join(embedded::SHA256);
    let marker = entry.join(".complete");
    let before = fs::metadata(&marker).expect("build.rs prepared the shared native entry");
    let manifest: serde_json::Value = serde_json::from_slice(&fs::read(&marker).unwrap()).unwrap();
    let library = entry
        .join(manifest["native_dir"].as_str().unwrap())
        .join(embedded::LIBRARY);
    let inode = fs::metadata(&library).unwrap().ino();
    let temporary = tempfile::tempdir().unwrap();
    Seekdb::open(temporary.path().join("database"))
        .unwrap()
        .close()
        .unwrap();
    assert_eq!(fs::metadata(&library).unwrap().ino(), inode);
    assert_eq!(
        fs::metadata(&marker).unwrap().modified().unwrap(),
        before.modified().unwrap()
    );
}
