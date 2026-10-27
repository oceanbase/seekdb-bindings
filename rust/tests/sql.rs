use mysql::{prelude::Queryable, Conn, OptsBuilder};
use seekdb::Seekdb;

fn connect(database: &Seekdb) -> Conn {
    let endpoint = database.connection_options().unwrap();
    assert_eq!(endpoint.transport, "unix_socket");
    Conn::new(
        OptsBuilder::new()
            .socket(endpoint.unix_socket)
            .user(Some(endpoint.user))
            .pass(Some(""))
            .db_name(Some("test")),
    )
    .unwrap()
}

#[test]
fn sql_data_is_shared_between_lifecycle_handles() {
    let temporary = tempfile::tempdir().unwrap();
    let path = temporary.path().join("database with spaces 中文");
    let database = Seekdb::open(&path).unwrap();
    let other_handle = Seekdb::open(&path).unwrap();
    let mut connection = connect(&database);
    connection
        .query_drop("CREATE TABLE rust_lifecycle (id INT PRIMARY KEY, value VARCHAR(64))")
        .unwrap();
    connection
        .exec_drop(
            "INSERT INTO rust_lifecycle VALUES (?, ?)",
            (1, "hello from Rust"),
        )
        .unwrap();
    let value: Option<String> = connection
        .query_first("SELECT value FROM rust_lifecycle WHERE id = 1")
        .unwrap();
    assert_eq!(value.as_deref(), Some("hello from Rust"));
    drop(connection);
    database.close().unwrap();

    let mut connection = connect(&other_handle);
    let value: Option<String> = connection
        .query_first("SELECT value FROM rust_lifecycle WHERE id = 1")
        .unwrap();
    assert_eq!(value.as_deref(), Some("hello from Rust"));
    drop(connection);
    other_handle.close().unwrap();
}
