package com.oceanbase.seekdb.test;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import java.io.*;
import java.net.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
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
            if (timeout < 0) throw new IllegalArgumentException("connect timeout cannot be negative");
            if (timeout == 0) {
                connectLocal();
                socket.setSoTimeout(readTimeout);
                return;
            }

            CountDownLatch done = new CountDownLatch(1);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread connector = new Thread(() -> {
                try {
                    connectLocal();
                } catch (Throwable t) {
                    failure.set(t);
                } finally {
                    done.countDown();
                }
            }, "seekdb-local-socket-connect");
            connector.setDaemon(true);
            connector.start();

            final boolean connected;
            try {
                connected = done.await(timeout, TimeUnit.MILLISECONDS);
            } catch (InterruptedException e) {
                closeAfterFailedConnect();
                Thread.currentThread().interrupt();
                InterruptedIOException interrupted =
                        new InterruptedIOException("Interrupted while connecting to " + path);
                interrupted.initCause(e);
                throw interrupted;
            }
            if (!connected) {
                closeAfterFailedConnect();
                throw new SocketTimeoutException("Timed out connecting to " + path);
            }

            Throwable cause = failure.get();
            if (cause instanceof IOException) throw (IOException) cause;
            if (cause instanceof RuntimeException) throw (RuntimeException) cause;
            if (cause instanceof Error) throw (Error) cause;
            if (cause != null) throw new IOException("Failed to connect to " + path, cause);
            socket.setSoTimeout(readTimeout);
        }
        private void connectLocal() throws IOException {
            // LocalSocket.connect(endpoint, timeout) is unsupported by Android, so the
            // Socket contract's bounded form is implemented by the worker above.
            socket.connect(new LocalSocketAddress(path, LocalSocketAddress.Namespace.FILESYSTEM));
        }
        private void closeAfterFailedConnect() {
            try { socket.close(); }
            catch (IOException ignored) {}
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
