package com.oceanbase.seekdb;

/** Platform-neutral entry point for libseekdb + JDBC. */
public final class SeekDB {
    /** SeekDB server parameter controlling the MySQL TCP listener. */
    public static final String MYSQL_PORT_MODE = "mysql_port_mode";
    /** Value that disables the MySQL TCP listener (Unix socket remains available). */
    public static final String MYSQL_PORT_MODE_DISABLED = "disabled";

    static {
        System.loadLibrary("seekdb_jni");
    }

    private SeekDB() {}

    /** Point libseekdb at the seekdb executable copied from APK assets. */
    public static void setBinaryPath(String path) {
        nativeSetBinaryPath(path);
    }

    /** Open an instance. Android connections use Unix sockets; server parameters
     * follow libseekdb's startup semantics. */
    public static EmbeddedSeekDB open(String dbDir, int port, String... parameters) {
        if (port < 0 || port > 65535) {
            throw new IllegalArgumentException("port must be in 0..65535");
        }
        validateParameters(parameters);
        if (parameters != null) {
            for (int i = 0; i < parameters.length; i += 2) {
                if ("port".equals(parameters[i]))
                    throw new IllegalArgumentException("Use the port argument, not a parameter pair");
            }
        }
        String[] all = new String[2 + (parameters == null ? 0 : parameters.length)];
        all[0] = "port";
        all[1] = Integer.toString(port);
        if (parameters != null) {
            System.arraycopy(parameters, 0, all, 2, parameters.length);
        }
        long handle = nativeOpen(dbDir, all);
        return new EmbeddedSeekDB(handle);
    }

    /**
     * Open an embedded instance using only its Unix domain socket. The server is
     * started with mysql_port_mode=disabled, so it does not listen on a TCP port.
     */
    public static EmbeddedSeekDB openUnixSocket(String dbDir, String... parameters) {
        validateParameters(parameters);
        if (parameters != null) {
            for (int i = 0; i < parameters.length; i += 2) {
                if (MYSQL_PORT_MODE.equals(parameters[i]))
                    throw new IllegalArgumentException("openUnixSocket manages mysql_port_mode");
            }
        }
        int extra = parameters == null ? 0 : parameters.length;
        String[] all = new String[extra + 2];
        if (extra > 0) {
            System.arraycopy(parameters, 0, all, 0, extra);
        }
        all[extra] = MYSQL_PORT_MODE;
        all[extra + 1] = MYSQL_PORT_MODE_DISABLED;
        return open(dbDir, 0, all);
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
    static native ConnectionOptions nativeConnectionOptions(long handle);
    static native void nativeClose(long handle);
}
