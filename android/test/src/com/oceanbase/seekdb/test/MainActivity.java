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
            String dbDir = getNoBackupFilesDir().getAbsolutePath() + "/seekdb-test";
            try (EmbeddedSeekDB db = SeekDB.openUnixSocket(dbDir)) {
            ConnectionOptions options = db.connectionOptions();
            try (Connection c = db.connect("test");
                 Statement s = c.createStatement();
                 ResultSet rs = s.executeQuery("SELECT 1")) {
                rs.next();
                final String result = "OK transport=" + options.transport + " port=" + options.port
                        + " value=" + rs.getInt(1);
                android.util.Log.i("SeekDBTest", result);
                runOnUiThread(() -> text.setText(result));
            }
            try (Connection c = db.connect("test")) {
                final String hybrid = HybridScenario.run(c);
                android.util.Log.i("SeekDBTest", hybrid);
                runOnUiThread(() -> text.setText(hybrid));
            }
            }
        } catch (Throwable t) {
            android.util.Log.e("SeekDBTest", "Test failed", t);
            final String result = "FAILED: " + t;
            runOnUiThread(() -> text.setText(result));
        }
    }
}
