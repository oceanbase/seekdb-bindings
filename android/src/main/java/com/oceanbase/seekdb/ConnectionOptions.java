package com.oceanbase.seekdb;

/** The connection endpoint returned by libseekdb's seekdb_connection_options. */
public final class ConnectionOptions {
    public final String transport;
    public final int port;
    public final String endpoint;
    public final String user;

    public ConnectionOptions(String transport, int port, String endpoint, String user) {
        this.transport = transport;
        this.port = port;
        this.endpoint = endpoint;
        this.user = user;
    }

    public String jdbcUrl(String database) {
        String db = database == null ? "" : database;
        if ("tcp".equals(transport)) {
            return "jdbc:mariadb://127.0.0.1:" + port + "/" + db + "?useSsl=false";
        }
        if ("unix_socket".equals(transport)) {
            try {
                return "jdbc:mariadb://localhost/" + db
                        + "?socketFactory=com.oceanbase.seekdb.AndroidSocketFactory&seekdbSocket="
                        + java.net.URLEncoder.encode(endpoint, "UTF-8") + "&sslMode=disable";
            } catch (java.io.UnsupportedEncodingException e) { throw new AssertionError(e); }
        }
        throw new UnsupportedOperationException("JDBC Android transport is unsupported: " + transport);
    }
}
