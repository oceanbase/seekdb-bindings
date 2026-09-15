package com.oceanbase.seekdb;

/**
 * Opens and manages a local SeekDB instance through libseekdb, independently of
 * the client driver used to connect to it.
 *
 * <pre>{@code
 * String dataDir = "/path/to/writable/persistent/database";
 * try (SeekDB db = SeekDB.open(dataDir, "mysql_port_mode", "disabled")) {
 *     ConnectionOptions options = db.connectionOptions();
 *     // Configure your chosen client using options.transport, options.user,
 *     // and options.unix_socket, options.named_pipe, or options.host/options.port.
 *     // Perform queries and close client connections before leaving this block.
 * }
 * }</pre>
 *
 * <p>Parameters are optional key/value pairs passed to libseekdb; omitting them
 * uses its defaults. This class does not construct JDBC URLs or load JDBC drivers.
 * The matching JNI and libseekdb libraries must be installed and discoverable by
 * the native loader. libseekdb locates the server executable beside itself:
 * {@code seekdb.exe} on Windows, otherwise {@code seekdb} with a fallback to
 * {@code libseekdb_exec.so} for APK-style packaging.
 *
 * <p>Use a writable, persistent data directory (app-private storage on Android).
 * Opening may start a server and wait for readiness, so do not open on the Android
 * UI thread. Keep this handle open while using its connection information; Unix
 * socket aliases may be process-local and become invalid when the handle closes.
 * Use try-with-resources to release the handle even when an operation fails.
 */
public final class SeekDB implements AutoCloseable {
    private long handle;
    static {
        System.loadLibrary("seekdb_jni");
    }

    private SeekDB(long handle) {
        this.handle = handle;
    }

    /** Open an instance with optional key/value parameter pairs, passed unchanged
     * to libseekdb. For example: open(path, "mysql_port_mode", "disabled").
     * Port, transport defaults and parameter values follow libseekdb/server semantics.
     *
     * @param dbDir writable, persistent database directory
     * @param parameters optional alternating parameter names and values
     * @return an open instance handle that must be closed after use
     * @throws IllegalArgumentException if parameter pairs are malformed
     * @throws RuntimeException if native open fails; the message includes its
     *         return code and error description
     */
    public static SeekDB open(String dbDir, String... parameters) {
        validateParameters(parameters);
        return new SeekDB(nativeOpen(dbDir, parameters));
    }

    /**
     * Returns native connection fields without choosing a client driver.
     * Unused transport fields are null; do not use a local endpoint from another
     * process or to establish new connections after closing this handle.
     *
     * @return the connection information for this instance
     * @throws IllegalStateException if this handle has been closed
     */
    public synchronized ConnectionOptions connectionOptions() {
        if (handle == 0) {
            throw new IllegalStateException("SeekDB is closed");
        }
        return nativeConnectionOptions(handle);
    }

    /**
     * Releases this handle and its native resources. Repeated calls are harmless.
     * Close client connections first. This does not delete the database files
     * or guarantee immediate server-process termination.
     */
    @Override
    public synchronized void close() {
        if (handle != 0) {
            nativeClose(handle);
            handle = 0;
        }
    }

    private static void validateParameters(String[] parameters) {
        if (parameters == null) return;
        if (parameters.length % 2 != 0)
            throw new IllegalArgumentException("Parameters must be key/value pairs");
        for (int i = 0; i < parameters.length; i++) {
            if (parameters[i] == null || (i % 2 == 0 && parameters[i].isEmpty()))
                throw new IllegalArgumentException("Parameter keys must be non-empty and values non-null");
        }
    }

    private static native long nativeOpen(String dbDir, String[] parameters);
    private static native ConnectionOptions nativeConnectionOptions(long handle);
    private static native void nativeClose(long handle);
}
