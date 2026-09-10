package com.oceanbase.seekdb.test;

import com.oceanbase.seekdb.ConnectionOptions;
import com.oceanbase.seekdb.EmbeddedSeekDB;
import com.oceanbase.seekdb.SeekDB;
import java.io.File;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;

/** Standalone ART entry point; no Activity or APK build is required. */
public final class Main {
    public static void main(String[] args) {
        try {
            if (args.length != 2) {
                throw new IllegalArgumentException("Usage: Main <seekdb-executable> <database-dir>");
            }
            System.out.println("VM=" + System.getProperty("java.vm.name")
                    + " uid=" + android.system.Os.getuid());
            SeekDB.setBinaryPath(args[0]);
            String endpoint;
            try (EmbeddedSeekDB db = SeekDB.openUnixSocket(args[1])) {
                ConnectionOptions options = db.connectionOptions();
                endpoint = options.endpoint;
                if (!"unix_socket".equals(options.transport) || options.port != 0
                        || !endpoint.startsWith("/proc/self/fd/")) {
                    throw new AssertionError("Unexpected connection options: " + endpoint);
                }
                try (Connection c = db.connect("test");
                     Statement s = c.createStatement();
                     ResultSet rs = s.executeQuery("SELECT 1")) {
                    if (!rs.next() || rs.getInt(1) != 1) {
                        throw new AssertionError("SELECT 1 failed");
                    }
                    System.out.println("JDBC_OK endpoint=" + endpoint + " port=" + options.port);
                }
                try (Connection c = db.connect("test")) {
                    System.out.println(HybridScenario.run(c));
                }
            }
            String directoryReference = endpoint.substring(0, endpoint.lastIndexOf('/'));
            if (new File(directoryReference).exists()) {
                throw new AssertionError("Directory FD still open after handle close");
            }
            System.out.println("APP_PROCESS_OK directory_fd_closed=true realPathBytes="
                    + (args[1] + "/run/sql.sock").getBytes("UTF-8").length);
        } catch (Throwable t) {
            t.printStackTrace();
            System.exit(1);
        }
    }
}
