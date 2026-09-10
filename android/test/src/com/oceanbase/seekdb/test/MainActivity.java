package com.oceanbase.seekdb.test;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import com.oceanbase.seekdb.ConnectionOptions;
import com.oceanbase.seekdb.EmbeddedSeekDB;
import com.oceanbase.seekdb.SeekDB;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;

public final class MainActivity extends Activity {
    private TextView text;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this);
        text.setText("running...");
        setContentView(text);
        new Thread(this::runTest).start();
    }

    private void runTest() {
        try {
            SeekDB.setBinaryPath(getApplicationInfo().nativeLibraryDir + "/libseekdb_exec.so");
            // Keep the database entirely in app-owned persistent storage;
            // never use /tmp, cache, or a system directory for test data.
            String dbDir = getNoBackupFilesDir().getAbsolutePath()
                    + "/seekdb-long-path-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz"
                    + "-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz";
            if ((dbDir + "/run/sql.sock").getBytes("UTF-8").length <= 108) {
                throw new AssertionError("Test requires a long socket path");
            }
            String endpoint;
            try (EmbeddedSeekDB db = SeekDB.openUnixSocket(dbDir)) {
            ConnectionOptions options = db.connectionOptions();
            endpoint = options.endpoint;
            if (!endpoint.startsWith("/proc/self/fd/") || options.port != 0) {
                throw new AssertionError("Unexpected endpoint: " + endpoint);
            }
            try (Connection c = db.connect("test");
                 Statement s = c.createStatement();
                 ResultSet rs = s.executeQuery("SELECT 1")) {
                rs.next();
                final String result = "OK transport=" + options.transport + " port=" + options.port
                        + " value=" + rs.getInt(1) + " endpoint=" + endpoint
                        + " realPathBytes=" + (dbDir + "/run/sql.sock").getBytes("UTF-8").length;
                android.util.Log.i("SeekDBTest", result);
                runOnUiThread(() -> text.setText(result));
            }
            try (Connection c = db.connect("test")) {
                final String hybrid = HybridScenario.run(c);
                android.util.Log.i("SeekDBTest", hybrid);
                runOnUiThread(() -> text.setText(hybrid));
            }
            }
            String directoryReference = endpoint.substring(0, endpoint.lastIndexOf('/'));
            if (new java.io.File(directoryReference).exists()) {
                throw new AssertionError("Directory FD still open after handle close");
            }
            android.util.Log.i("SeekDBTest", "DIRECTORY_FD_CLOSED_OK");
        } catch (Throwable t) {
            android.util.Log.e("SeekDBTest", "Test failed", t);
            final String result = "FAILED: " + t;
            runOnUiThread(() -> text.setText(result));
        }
    }
}
