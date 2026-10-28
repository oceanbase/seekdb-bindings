use mysql::{prelude::Queryable, Conn, OptsBuilder};

/// Open and close a database through a transitive seekdb dependency.
///
/// # Safety
/// `directory` must point to a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn seekdb_consumer_probe(directory: *const std::ffi::c_char) -> i32 {
    if directory.is_null() {
        return -1;
    }
    let Ok(directory) = std::ffi::CStr::from_ptr(directory).to_str() else {
        return -1;
    };
    match seekdb_consumer_b::open(std::path::Path::new(directory))
        .map_err(|error| error.to_string())
        .and_then(|database| {
            let endpoint = database.connection_options().map_err(|e| e.to_string())?;
            let options = OptsBuilder::new()
                .ip_or_hostname(Some(endpoint.host.unwrap_or_else(|| "127.0.0.1".into())))
                .tcp_port(endpoint.port.unwrap_or(0))
                .socket(endpoint.unix_socket)
                .user(Some(endpoint.user))
                .pass(Some(""))
                .db_name(Some("test"));
            let mut connection = Conn::new(options).map_err(|e| e.to_string())?;
            let value: Option<u64> = connection
                .query_first("SELECT 1")
                .map_err(|e| e.to_string())?;
            if value != Some(1) {
                return Err(format!("unexpected query result: {value:?}"));
            }
            drop(connection);
            database.close().map_err(|e| e.to_string())
        }) {
        Ok(()) => 0,
        Err(error) => {
            eprintln!("{error}");
            -1
        }
    }
}
