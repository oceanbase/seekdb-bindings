package com.oceanbase.seekdb.test;

import com.oceanbase.seekdb.ConnectionOptions;
import java.net.URLEncoder;
import java.sql.Connection;
import java.sql.DriverManager;

/** Example-only MariaDB JDBC configuration, not part of the public SeekDB API. */
final class JdbcExample {
    static Connection connect(ConnectionOptions options, String database) throws Exception {
        if (!"unix_socket".equals(options.transport) || options.unix_socket == null) {
            throw new IllegalArgumentException("This Android example requires a Unix socket");
        }
        Class.forName("org.mariadb.jdbc.Driver");
        String url = "jdbc:mariadb://localhost/" + URLEncoder.encode(database, "UTF-8")
                + "?socketFactory=com.oceanbase.seekdb.test.AndroidSocketFactory"
                + "&seekdbSocket=" + URLEncoder.encode(options.unix_socket, "UTF-8")
                + "&sslMode=disable";
        return DriverManager.getConnection(url, options.user, "");
    }
}
