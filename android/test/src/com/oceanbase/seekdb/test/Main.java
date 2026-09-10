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
            try {
                SeekDB.open(args[0], "mysql_port_mode", "disabled"); // not a database directory
                throw new AssertionError("Expected invalid directory failure");
            } catch (RuntimeException expected) {
                if (!expected.getMessage().contains("directory")
                        || !expected.getMessage().contains("errno=")) {
                    throw new AssertionError("Missing native error detail", expected);
                }
                System.out.println("OPEN_ERROR_OK " + expected.getMessage());
            }
            SeekDB.setBinaryPath(args[0] + ".missing");
            try {
                SeekDB.open(args[1] + "-spawn-error", "mysql_port_mode", "disabled");
                throw new AssertionError("Expected missing executable failure");
            } catch (RuntimeException expected) {
                if (!expected.getMessage().contains("Cannot execute")
                        || !expected.getMessage().contains("errno=2")) {
                    throw new AssertionError("Missing spawn error detail", expected);
                }
                System.out.println("SPAWN_ERROR_OK " + expected.getMessage());
            } finally {
                SeekDB.setBinaryPath(args[0]);
            }
            String endpoint;
            try (EmbeddedSeekDB db = SeekDB.open(args[1], "mysql_port_mode", "disabled", "port", "0")) {
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
