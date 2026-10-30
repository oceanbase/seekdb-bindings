use mysql::{prelude::Queryable, Conn, OptsBuilder};
use std::path::PathBuf;

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let directory = PathBuf::from(std::env::args_os().nth(1).ok_or("database path required")?);
    let first = seekdb_consumer_b::open(&directory)?;
    let second = seekdb_consumer_c::open(&directory)?;
    let endpoint = first.connection_options()?;
    let mut connection = Conn::new(
        OptsBuilder::new()
            .ip_or_hostname(Some(endpoint.host.unwrap_or_else(|| "127.0.0.1".into())))
            .tcp_port(endpoint.port.unwrap_or(0))
            .socket(endpoint.unix_socket)
            .user(Some(endpoint.user))
            .pass(Some(""))
            .db_name(Some("test")),
    )?;
    let value: Option<u64> = connection.query_first("SELECT 1")?;
    assert_eq!(value, Some(1));
    println!("B and C share seekdb: SELECT 1 = 1");
    drop(connection);
    second.close()?;
    first.close()?;
    Ok(())
}
