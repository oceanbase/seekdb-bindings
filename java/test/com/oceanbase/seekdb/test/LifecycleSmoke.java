package com.oceanbase.seekdb.test;

import com.oceanbase.seekdb.ConnectionOptions;
import com.oceanbase.seekdb.SeekDB;

/** Exercises the packaged JAR, JNI, driver and engine together. */
public final class LifecycleSmoke {
    public static void main(String[] args) {
        SeekDB db = SeekDB.open(args[0]);
        try {
            ConnectionOptions options = db.connectionOptions();
            boolean valid = "unix_socket".equals(options.transport)
                ? options.unix_socket != null && !options.unix_socket.isEmpty()
                : "named_pipe".equals(options.transport)
                    ? options.named_pipe != null && !options.named_pipe.isEmpty()
                    : "tcp".equals(options.transport) && options.host != null && options.port > 0;
            if (!valid || options.user == null || options.user.isEmpty()) {
                throw new AssertionError("Packaged JNI returned an invalid connection endpoint");
            }
            System.out.println("Packaged Java/JNI opened seekdb using " + options.transport);
        } finally {
            db.close();
        }
        db.close();
        try {
            db.connectionOptions();
            throw new AssertionError("A closed Java handle still returned connection options");
        } catch (IllegalStateException expected) {
            // Closing must invalidate the native handle exposed through Java.
        }
    }
}
