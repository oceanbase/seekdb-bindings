package com.oceanbase.seekdb;

/** Platform-neutral entry point for libseekdb. */
public final class SeekDB implements AutoCloseable {
    private long handle;
    static {
        System.loadLibrary("seekdb_jni");
    }

    private SeekDB(long handle) {
        this.handle = handle;
    }

    /** Optional override of libseekdb's platform-specific executable discovery. */
    public static void setBinaryPath(String path) {
        nativeSetBinaryPath(path);
    }

    /** Open an instance with optional key/value parameter pairs, passed unchanged
     * to libseekdb. For example: open(path, "mysql_port_mode", "disabled").
     * Port, transport defaults and parameter values follow libseekdb/server semantics.
     */
    public static SeekDB open(String dbDir, String... parameters) {
        validateParameters(parameters);
        return new SeekDB(nativeOpen(dbDir, parameters));
    }

    public synchronized ConnectionOptions connectionOptions() {
        if (handle == 0) {
            throw new IllegalStateException("SeekDB is closed");
        }
        return nativeConnectionOptions(handle);
    }

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

    private static native void nativeSetBinaryPath(String path);
    private static native long nativeOpen(String dbDir, String[] parameters);
    private static native ConnectionOptions nativeConnectionOptions(long handle);
    private static native void nativeClose(long handle);
}
