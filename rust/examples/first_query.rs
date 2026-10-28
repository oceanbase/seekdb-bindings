use mysql::{prelude::Queryable, Conn, OptsBuilder};
use seekdb::Seekdb;
use std::error::Error;
use std::path::PathBuf;

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), Box<dyn Error + Send + Sync>> {
    let directory = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .ok_or("usage: first_query <database-directory>")?;
    let database = Seekdb::open(directory)?;
    let endpoint = database.connection_options()?;
    // POSIX defaults to a Unix socket; Windows defaults to a random TCP port.
    // Use the returned endpoint rather than assuming port 3306.
    let options = OptsBuilder::new()
        .ip_or_hostname(Some(endpoint.host.unwrap_or_else(|| "127.0.0.1".into())))
        .tcp_port(endpoint.port.unwrap_or(0))
        .socket(endpoint.unix_socket)
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
