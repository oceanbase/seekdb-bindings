package com.oceanbase.seekdb.test;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import java.io.*;
import java.net.*;
import org.mariadb.jdbc.Configuration;
import org.mariadb.jdbc.util.ConfigurableSocketFactory;

/** Example-only MariaDB JDBC adapter for Android filesystem Unix domain sockets. */
public final class AndroidSocketFactory extends ConfigurableSocketFactory {
    private String path;
    @Override public void setConfiguration(Configuration config, String host) {
        path = config.nonMappedOptions().getProperty("seekdbSocket");
        if (path != null) {
            try { path = URLDecoder.decode(path, "UTF-8"); }
            catch (UnsupportedEncodingException e) { throw new AssertionError(e); }
        }
    }
    @Override public Socket createSocket() throws IOException {
        if (path == null) throw new IOException("seekdbSocket is required");
        return new UnixSocket(path);
    }
    @Override public Socket createSocket(String h, int p) throws IOException { return createSocket(); }
    @Override public Socket createSocket(String h, int p, InetAddress l, int lp) throws IOException { return createSocket(); }
    @Override public Socket createSocket(InetAddress h, int p) throws IOException { return createSocket(); }
    @Override public Socket createSocket(InetAddress h, int p, InetAddress l, int lp) throws IOException { return createSocket(); }

    private static final class UnixSocket extends Socket {
        private final LocalSocket socket = new LocalSocket();
        private final String path;
        private int readTimeout;
        UnixSocket(String path) { this.path = path; }
        @Override public void connect(SocketAddress ignored, int timeout) throws IOException {
            // Android's overload with a timeout is unimplemented.
            socket.connect(new LocalSocketAddress(path, LocalSocketAddress.Namespace.FILESYSTEM));
            socket.setSoTimeout(readTimeout);
        }
        @Override public void connect(SocketAddress ignored) throws IOException { connect(ignored, 0); }
        @Override public InputStream getInputStream() throws IOException { return socket.getInputStream(); }
        @Override public OutputStream getOutputStream() throws IOException { return socket.getOutputStream(); }
        @Override public boolean isConnected() { return socket.isConnected(); }
        @Override public boolean isClosed() { return socket.isClosed(); }
        @Override public void close() throws IOException { socket.close(); }
        @Override public void setTcpNoDelay(boolean on) {}
        @Override public void setKeepAlive(boolean on) {}
        @Override public void setSoLinger(boolean on, int seconds) {}
        @Override public void setSoTimeout(int timeout) throws SocketException {
            readTimeout = timeout;
            if (!socket.isConnected()) return;
            try { socket.setSoTimeout(timeout); }
            catch (IOException e) { throw new SocketException(e.toString()); }
        }
    }
}
