use seekdb::Seekdb;

#[test]
fn open_endpoint_and_close() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("database");
    let database = Seekdb::open(&path).unwrap();
    let endpoint = database.connection_options().unwrap();
    assert_eq!(endpoint.user, "root");
    assert!(["tcp", "unix_socket", "named_pipe"].contains(&endpoint.transport.as_str()));
    assert!(database.directory().is_absolute());
    database.close().unwrap();
}
