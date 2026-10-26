use mysql::{prelude::Queryable, Conn, OptsBuilder};
use seekdb::{set_runtime_cache_dir, Error, Seekdb};
use std::fs;
use std::path::{Path, PathBuf};

fn find_bundle(directory: &Path) -> Option<PathBuf> {
    if directory.join(".complete").is_file() {
        return Some(directory.to_path_buf());
    }
    for entry in fs::read_dir(directory).ok()? {
        let entry = entry.ok()?;
        if entry.file_type().ok()?.is_dir() {
            if let Some(path) = find_bundle(&entry.path()) {
                return Some(path);
            }
        }
    }
    None
}

#[test]
fn api_configuration_is_frozen_and_cache_recovers_with_library_loaded() {
    let temporary = tempfile::tempdir().unwrap();
    let root = temporary.path().join("runtime cache 中文");
    set_runtime_cache_dir(&root).unwrap();
    let data = temporary.path().join("database");
    let database = Seekdb::open(&data).unwrap();
    assert!(matches!(
        set_runtime_cache_dir(temporary.path().join("other")),
        Err(Error::InvalidInput(_))
    ));
    let endpoint = database.connection_options().unwrap();
    let mut connection = Conn::new(
        OptsBuilder::new()
            .socket(endpoint.unix_socket)
            .user(Some(endpoint.user))
            .pass(Some(""))
            .db_name(Some("test")),
    )
    .unwrap();
    connection
        .query_drop("CREATE TABLE cache_recovery (value INT)")
        .unwrap();
    connection
        .query_drop("INSERT INTO cache_recovery VALUES (42)")
        .unwrap();

    let extracted = find_bundle(&root).expect("API-selected root contains the extracted bundle");
    let manifest: serde_json::Value =
        serde_json::from_slice(&fs::read(extracted.join(".complete")).unwrap()).unwrap();
    let native_dir = extracted.join(manifest["native_dir"].as_str().unwrap());
    fs::remove_file(native_dir.join("seekdb")).unwrap();
    let repaired = Seekdb::open(&data).unwrap();
    assert!(native_dir.join("seekdb").is_file());
    let value: Option<u64> = connection
        .query_first("SELECT value FROM cache_recovery")
        .unwrap();
    assert_eq!(value, Some(42));
    repaired.close().unwrap();

    fs::remove_dir_all(&extracted).unwrap();
    // Already mapped code and the active server remain usable on Linux. The
    // next open must restore files even though Native's OnceLock is populated.
    let value: Option<u64> = connection
        .query_first("SELECT value FROM cache_recovery")
        .unwrap();
    assert_eq!(value, Some(42));
    let other = Seekdb::open(&data).unwrap();
    assert!(extracted.join(".complete").is_file());
    let value: Option<u64> = connection
        .query_first("SELECT value FROM cache_recovery")
        .unwrap();
    assert_eq!(value, Some(42));
    drop(connection);
    other.close().unwrap();
    database.close().unwrap();
}
