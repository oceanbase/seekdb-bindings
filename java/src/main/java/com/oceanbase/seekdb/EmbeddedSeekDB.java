package com.oceanbase.seekdb;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.SQLException;

/** A libseekdb-managed embedded instance exposed through JDBC. */
public final class EmbeddedSeekDB implements AutoCloseable {
    private long handle;

    EmbeddedSeekDB(long handle) {
        this.handle = handle;
    }

    public ConnectionOptions connectionOptions() {
        ensureOpen();
        return SeekDB.nativeConnectionOptions(handle);
    }

    public Connection connect(String database) throws SQLException {
        return connect(database, null);
    }

    /** Supply a platform-specific JDBC socket factory for Unix socket connections. */
    public Connection connect(String database, String socketFactory) throws SQLException {
        try { Class.forName("org.mariadb.jdbc.Driver"); }
        catch (ClassNotFoundException e) { throw new SQLException("MariaDB JDBC driver is missing", e); }
        ConnectionOptions options = connectionOptions();
        return DriverManager.getConnection(options.jdbcUrl(database, socketFactory), options.user, "");
    }

    @Override
    public void close() {
        if (handle != 0) {
            SeekDB.nativeClose(handle);
            handle = 0;
        }
    }

    private void ensureOpen() {
        if (handle == 0) {
            throw new IllegalStateException("SeekDB is closed");
        }
    }
}
