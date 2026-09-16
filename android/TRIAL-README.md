# SeekDB Android Trial Kit

## Contents and support scope

- `aar/seekdb-android.aar`: the public Java API, JNI library, libseekdb, and the
  SeekDB executable.
- `examples/aar-consumer/`: a standalone Gradle example that depends only on
  the local AAR.
- `demo/seekdb-test.apk`: a trial APK built from the example. It automatically
  runs a query and a hybrid-search scenario when launched.
- `licenses/`: licenses and third-party notices included with the kit. See the
  test report for the exact build revisions and verification results.
- Only Android ARM64 (`arm64-v8a`) is supported. The native minimum API level
  is 28.
- This is a trial package. It has not been certified for every Android version,
  physical device, or production workload.
- The example uses targetSdk 28. Higher targetSdk levels and app-store
  distribution require separate validation.

## Integrate into your project

1. Copy the AAR into the application module's `libs/` directory and add
   `implementation files('libs/seekdb-android.aar')`.
2. Do not also depend on `seekdb-java.jar`, because that would introduce
   duplicate classes.
3. Set `android.packaging.jniLibs.useLegacyPackaging = true` so the native files
   are extracted to the filesystem. Do not disable extraction: SeekDB must
   launch its child process from a real filesystem path.
4. Use a persistent, app-private directory, such as a child directory of
   `context.getNoBackupFilesDir()`, and call the API from a background thread.

```java
String path = new java.io.File(context.getNoBackupFilesDir(), "seekdb").getAbsolutePath();
try (com.oceanbase.seekdb.SeekDB db = com.oceanbase.seekdb.SeekDB.open(path)) {
    com.oceanbase.seekdb.ConnectionOptions options = db.connectionOptions();
    // Use the client driver of your choice and connect with fields such as
    // options.unix_socket. Close the client connection before closing db.
}
```

No binary path configuration is required. libseekdb looks for `seekdb` in its
own directory and falls back to `libseekdb_exec.so` when it is not present. The
latter is an executable named to work with APK native-file extraction; it is
not a shared library. The JNI library and libseekdb itself are shared libraries.
The AAR does not contain a JDBC driver or test code.

## Build and run the example

Install SDK platform 36.1, Build Tools 36.1.0, JDK 17 or a compatible version,
and Gradle 8.13. The example uses Android Gradle Plugin 8.13.2. The repository
does not include a Gradle Wrapper. Set `ANDROID_HOME` to your SDK directory, or
set `sdk.dir` in the example's `local.properties` file.

```sh
cd examples/aar-consumer
gradle :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.oceanbase.seekdb.trial/com.oceanbase.seekdb.test.MainActivity
adb logcat -s SeekDBTest:I
```

Expected output includes `OK transport=unix_socket port=0 value=1`,
`HYBRID_OK`, and `DIRECTORY_FD_CLOSED_OK`. The database is stored in the trial
app's private directory; `/tmp` is not used. The test recreates its own
`android_hybrid_demo` table. Do not point it at an instance containing important
data.

The example uses MariaDB JDBC 3.5.6 and includes its JAR. `JdbcExample` and
`AndroidSocketFactory` are example configuration and adapter code specific to
that driver; they are not part of the general bindings API. Other drivers
should use their own Unix-domain-socket connection mechanism. If an adapter is
loaded through reflection and code shrinking is enabled, preserve the adapter's
class name. The trial example does not enable code shrinking. The AAR includes
keep rules for the public JNI API.

## Notes

The first launch initializes the database, so it can take some time and consume
several hundred megabytes of storage. Do not start it on the UI thread. Closing
the handle does not guarantee that the service process exits immediately, and
it does not delete the database. A `/proc/self/fd/` socket address is valid only
within the corresponding process and handle lifetime. Insufficient device
storage can cause installation to fail; do not delete personal data merely to
make room for this test.
