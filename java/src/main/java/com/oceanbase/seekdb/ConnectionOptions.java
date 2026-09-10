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

}
