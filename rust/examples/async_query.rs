use mysql_async::{prelude::Queryable, OptsBuilder, Pool};
use seekdb::{ConnectionOptions, Seekdb};
use std::error::Error;
use std::path::PathBuf;

type AppError = Box<dyn Error + Send + Sync>;

async fn query(endpoint: ConnectionOptions) -> Result<(), AppError> {
    let socket = endpoint
        .unix_socket
        .ok_or("expected a Unix socket endpoint")?;
    let options = OptsBuilder::default()
        .socket(Some(socket))
        .user(Some(endpoint.user))
        .pass(Some(""))
        .db_name(Some("test"));
    let pool = Pool::new(options);
    let result = async {
        let mut connection = pool.get_conn().await?;
        let value: Option<u64> = connection.query_first("SELECT 1").await?;
        if value != Some(1) {
            return Err(format!("unexpected query result: {value:?}").into());
        }
        println!("SELECT 1 = {}", value.unwrap());
        Ok::<(), AppError>(())
    }
    .await;
    // Disconnect the whole pool, including idle connections, before db.close().
    let disconnect = pool.disconnect().await;
    result?;
    disconnect?;
    Ok(())
}

#[tokio::main]
async fn main() {
    if let Err(error) = run().await {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

async fn run() -> Result<(), AppError> {
    let directory = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .ok_or("usage: async_query <database-directory>")?;
    tokio::task::spawn_blocking(move || -> Result<(), AppError> {
        // Seekdb stays on this thread: it is neither Send nor Sync. In
        // particular, do not return it from spawn_blocking. Only the copied
        // ConnectionOptions enter the async SQL future.
        let database = Seekdb::open(directory)?;
        let endpoint = database.connection_options()?;
        let result = tokio::runtime::Handle::current().block_on(query(endpoint));
        let close = database.close();
        result?;
        close?;
        Ok(())
    })
    .await??;
    Ok(())
}
