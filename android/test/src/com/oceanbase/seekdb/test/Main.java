package com.oceanbase.seekdb.test;

import com.oceanbase.seekdb.ConnectionOptions;
import com.oceanbase.seekdb.SeekDB;
import java.io.File;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;

/** Standalone ART entry point; no Activity or APK build is required. */
public final class Main {
    public static void main(String[] args) {
        try {
            if (args.length != 1) {
                throw new IllegalArgumentException("Usage: Main <database-dir>");
            }
            System.out.println("VM=" + System.getProperty("java.vm.name")
                    + " uid=" + android.system.Os.getuid());
            File notDirectory = new File(args[0] + "-not-a-directory");
            if (!notDirectory.createNewFile() && !notDirectory.isFile()) {
                throw new AssertionError("Invalid directory fixture must be a regular file");
            }
            try {
                SeekDB.open(notDirectory.getAbsolutePath(), "mysql_port_mode", "disabled");
                throw new AssertionError("Expected invalid directory failure");
            } catch (RuntimeException expected) {
                if (!expected.getMessage().contains("directory")
                        || !expected.getMessage().contains("errno=")) {
                    throw new AssertionError("Missing native error detail", expected);
                }
                System.out.println("OPEN_ERROR_OK " + expected.getMessage());
            }
            String endpoint;
            try (SeekDB db = SeekDB.open(args[0], "mysql_port_mode", "disabled", "port", "0")) {
                System.out.println("AUTO_BINARY_PATH_OK no override supplied");
                ConnectionOptions options = db.connectionOptions();
                endpoint = options.unix_socket;
                if (!"unix_socket".equals(options.transport) || options.port != 0
                        || options.host != null || options.named_pipe != null
                        || !endpoint.startsWith("/proc/self/fd/")) {
                    throw new AssertionError("Unexpected connection options: " + endpoint);
                }
                try (Connection c = JdbcExample.connect(options, "test");
                     Statement s = c.createStatement();
                     ResultSet rs = s.executeQuery("SELECT 1")) {
                    if (!rs.next() || rs.getInt(1) != 1) {
                        throw new AssertionError("SELECT 1 failed");
                    }
                    System.out.println("JDBC_OK endpoint=" + endpoint + " port=" + options.port);
                }
                try (Connection c = JdbcExample.connect(options, "test")) {
                    System.out.println(HybridScenario.run(c));
                }
                db.close();
                db.close();
                try {
                    db.connectionOptions();
                    throw new AssertionError("Closed handle must reject connectionOptions");
                } catch (IllegalStateException expected) {
                    System.out.println("LIFECYCLE_OK repeated close and use-after-close guard");
                }
            }
            String directoryReference = endpoint.substring(0, endpoint.lastIndexOf('/'));
            if (new File(directoryReference).exists()) {
                throw new AssertionError("Directory FD still open after handle close");
            }
            System.out.println("APP_PROCESS_OK directory_fd_closed=true realPathBytes="
                    + (args[0] + "/run/sql.sock").getBytes("UTF-8").length);
        } catch (Throwable t) {
            t.printStackTrace();
            System.exit(1);
        }
    }
}
