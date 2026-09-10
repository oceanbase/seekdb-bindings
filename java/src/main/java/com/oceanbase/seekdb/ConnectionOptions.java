package com.oceanbase.seekdb;

/** The connection endpoint returned by libseekdb's seekdb_connection_options. */
public final class ConnectionOptions {
    public final String transport;
    public final int port;
    public final String host;
    public final String unix_socket;
    public final String named_pipe;
    public final String user;

    public ConnectionOptions(String transport, int port, String host, String unix_socket,
                             String named_pipe, String user) {
        this.transport = transport;
        this.port = port;
        this.host = host;
        this.unix_socket = unix_socket;
        this.named_pipe = named_pipe;
        this.user = user;
    }

    public String jdbcUrl(String database) {
        return jdbcUrl(database, null);
    }

    public String jdbcUrl(String database, String socketFactory) {
        String db = database == null ? "" : database;
        if ("tcp".equals(transport)) {
            String address = host != null && host.contains(":") && !host.startsWith("[")
                    ? "[" + host + "]" : host;
            return "jdbc:mariadb://" + address + ":" + port + "/" + db + "?sslMode=disable";
        }
        if ("unix_socket".equals(transport)) {
            if (socketFactory == null || socketFactory.isEmpty()) {
                throw new UnsupportedOperationException("Supply a platform-specific Unix socket factory");
            }
            try {
                return "jdbc:mariadb://localhost/" + db
                        + "?socketFactory=" + java.net.URLEncoder.encode(socketFactory, "UTF-8")
                        + "&seekdbSocket=" + java.net.URLEncoder.encode(unix_socket, "UTF-8")
                        + "&sslMode=disable";
            } catch (java.io.UnsupportedEncodingException e) { throw new AssertionError(e); }
        }
        throw new UnsupportedOperationException("No JDBC adapter configured for transport: " + transport);
    }
}
