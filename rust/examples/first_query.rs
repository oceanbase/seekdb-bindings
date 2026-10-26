use mysql::{prelude::Queryable, Conn, OptsBuilder};
use seekdb::Seekdb;
use std::error::Error;
use std::path::PathBuf;

fn main() -> Result<(), Box<dyn Error + Send + Sync>> {
    let directory = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .ok_or("usage: first_query <database-directory>")?;
    let database = Seekdb::open(directory)?;
    let endpoint = database.connection_options()?;
    let socket = endpoint
        .unix_socket
        .ok_or("expected a Unix socket endpoint")?;
    let options = OptsBuilder::new()
        .socket(Some(socket))
        .user(Some(endpoint.user))
        .pass(Some(""))
        .db_name(Some("test"));

    let mut connection = Conn::new(options)?;
    let value: Option<u64> = connection.query_first("SELECT 1")?;
    if value != Some(1) {
        return Err(format!("unexpected query result: {value:?}").into());
    }
    println!("SELECT 1 = {}", value.unwrap());

    // SQL clients must release their connections before the lifecycle handle.
    drop(connection);
    database.close()?;
    Ok(())
}
