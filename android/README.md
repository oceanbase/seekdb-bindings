# Android embedded Java/JDBC

Requires the platform-native connection support in PR #60. On Android the
server defaults to `mysql_port_mode=disabled`; connections use a Unix socket.

## Build

Initialize the repository submodules. Install Android SDK platform 36.1, Build
Tools 36.1.0, and NDK r27d (27.3.13750724). Use a JDK supporting `--release 8`.
Provide an ARM64/API28 SeekDB executable, an Android ARM64/API28 static OpenSSL
installation, and MariaDB JDBC 3.5.6. These inputs are not downloaded by the script.

```sh
export ANDROID_SDK_ROOT=/path/to/android-sdk
export SEEKDB_ANDROID_BIN=/path/to/android/seekdb
export ANDROID_OPENSSL_ROOT=/path/to/android/openssl
export MARIADB_JDBC_JAR=/path/to/mariadb-java-client-3.5.6.jar
BUILD_TEST_APK=1 bash android/build.sh
```

Outputs are in `build/android/package`: `seekdb-android.aar` (default; set BUILD_AAR=0
to skip), two JARs, `jniLibs/arm64-v8a` and optionally
a signed test APK. The script pins native API 28 and NDK r27d, including the
sysroot, and defaults to four build jobs. Set BUILD_JOBS to adjust concurrency.

## Use

For Android development, prefer the AAR: it includes the Java API and all three
native files, but no JDBC driver or test classes. Do not also add seekdb-java.jar.
Configure `packaging.jniLibs.useLegacyPackaging = true` in the consuming app so
the executable is extracted. JNI keep rules are included as proguard.txt.
See `TRIAL-README.md` and the standalone template in `examples/aar-consumer/`.

To stage a trial kit after building the AAR:

```sh
TRIAL_DIR=/new/path/seekdb-android-trial bash android/prepare-trial.sh
cd /new/path/seekdb-android-trial/examples/aar-consumer
./gradlew :app:assembleDebug
```

Install and test that consumer APK before copying it into the kit's demo directory.
The following manual JAR/native integration remains an alternative to the AAR.

For this MariaDB JDBC example, include `seekdb-java.jar` and the JDBC JAR as app
dependencies and copy all three native files to
`app/src/main/jniLibs/arm64-v8a`. Enable `android:extractNativeLibs="true"` and
Gradle legacy JNI packaging (`packaging { jniLibs { useLegacyPackaging = true } }`).
Preserve `com.oceanbase.seekdb.**` and `org.mariadb.jdbc.Driver` if enabling shrinking;
shrinking has not been tested. Android uses `/proc/self/fd/<directory-fd>/sql.sock`
to avoid the Unix socket address length limit even with a long database path.
The endpoint is process-local and may only be used to open connections while its
`SeekDB` handle remains open. The directory FD is closed with the handle
and is not inherited by the SeekDB executable. Run operations off the UI thread.

```java
String path = context.getNoBackupFilesDir().getAbsolutePath() + "/db";
try (SeekDB db = SeekDB.open(path, "mysql_port_mode", "disabled")) {
    ConnectionOptions options = db.connectionOptions();
    // Example for MariaDB JDBC 3.5.6 + our Android adapter, not generic JDBC options.
    Class.forName("org.mariadb.jdbc.Driver");
    String url = "jdbc:mariadb://localhost/test"
            + "?socketFactory=com.oceanbase.seekdb.test.AndroidSocketFactory"
            + "&seekdbSocket=" + java.net.URLEncoder.encode(options.unix_socket, "UTF-8")
            + "&sslMode=disable";
    try (java.sql.Connection connection =
            java.sql.DriverManager.getConnection(url, options.user, "")) {
        // Use JDBC normally; close connections before closing the instance handle.
    }
}
```

The URL prefix and `socketFactory`/`sslMode` options are MariaDB-driver-specific;
copy `test/src/com/oceanbase/seekdb/test/AndroidSocketFactory.java` into your app
to run this example (update the factory class name if you change its package).
This is example code, not a public bindings class or a separately published JAR.
`seekdbSocket` is consumed by this example's Android socket adapter. Other drivers
must use their own configuration and compatible transport adapter. The common
SeekDB API returns connection fields only; it has no JDBC URL or connect helper.

libseekdb looks beside its loaded library for `seekdb`, falling back to
`libseekdb_exec.so` if the ordinary name is missing. The latter name lets the APK
installer handle extraction as a native file; it remains an executable, not a
shared library. No Android-specific lookup branch or Java path setter is needed;
native files must still be extracted into the same directory.
Android spawns use POSIX_SPAWN_USEVFORK to avoid the
observed ART child fork-handler hang. The Java socket adapter uses LocalSocket;
Android's unimplemented connect-with-timeout overload is not used. Read timeouts
are applied after connecting. The adapter is intended for local Unix sockets.

## Tests and limitations

```sh
adb install build/android/package/seekdb-hybrid-test.apk
adb shell am start -n com.oceanbase.seekdb.pr60/com.oceanbase.seekdb.test.MainActivity
adb logcat -d -s SeekDBTest:I
```

The APK uses app-private persistent storage and only recreates its
`android_hybrid_demo` table. It exercises libseekdb open/connection_options,
JDBC SELECT 1, fulltext/HNSW indexes, inserts, refresh, disabled TCP configuration,
and server-generated RRF hybrid SQL. Expected output includes HYBRID_OK with five
distinct IDs including doc_1. The fixture follows pyseekdb's hybrid example using
the same 10 documents and deterministic explicit vectors, without a model download.
The database directory deliberately makes the real socket path exceed 108 bytes.
The test checks the returned `/proc/self/fd/` endpoint and reports
`DIRECTORY_FD_CLOSED_OK` after handle close releases the directory reference.

Validated on the ARM64 Android 16 emulator (API 36, 36.1 image), native min API 28,
APK targetSdk 28. Newer targetSdk settings and physical devices are not verified.
The proc-fd path passed JDBC and hybrid queries with SELinux Enforcing and a
186-byte real socket path.
The package is not a standalone pure-Java database; deploy the native files too.

### Standalone `main()` via app_process

After building the package above, with the matching debug test APK already
installed on the connected ARM64 emulator, run:

```sh
ANDROID_SDK_ROOT=/path/to/Android/sdk bash android/test/run-main.sh
```

This compiles a separate `Main` against the existing JARs, converts the test and
both dependency JARs to `build/android/standalone/seekdb-test-dex.jar`, and runs it
with `/system/bin/app_process`. It does not build, install, or launch an APK.
The existing debuggable package is only used for `run-as` access to persistent
private storage and its already-installed native binaries. Thus this recipe is
not an APK-free device provisioning method. Its SELinux domain is `runas_app`,
not the Activity's regular app domain, and it does not replace APK testing.

Observed pitfalls on API 36 (SELinux Enforcing):

- Writable DEX JARs abort ART startup before `main()`; deploy them read-only.
- D8 does not copy JAR resources. The runner preserves JDBC properties and
  `META-INF/services`, avoiding missing configuration and plugin metadata.
- Both `java.library.path` and native dependency resolution must point to the
  installed ARM64 libraries; a DEX JAR alone cannot run the database.
- Shell permissions differ from app permissions. This runner uses `run-as` and
  a dedicated `no_backup/app-process-db-...` directory, never temporary storage.
- `/proc/self/fd/` endpoints belong to the calling process and handle. Do not
  reuse them from another process or after closing the handle.

Validated initial creation with automatic executable discovery and a 162-byte real socket path:
`JDBC_OK`, `HYBRID_OK` (10 rows, five expected results, TCP disabled), and
`APP_PROCESS_OK directory_fd_closed=true`, with exit status 0.
The runner also checks descriptive native open errors for a non-directory data
path, and verifies that `unix_socket` is returned while
`host` and `named_pipe` remain null. Common Java/JNI code lives in `java/`; only
the LocalSocket adapter, Android build and tests remain here.

Additional manual validation ran the original pyseekdb simple, complete, seven
hybrid scenarios, and sparse examples against the APK-owned engine through a
test-only ADB/Unix socket relay. Python and real embedding/reranking models ran on
macOS, including BM25 and CrossEncoder. These validate engine compatibility with
the SDK workloads, not Python/model execution inside Android. Namespace examples
are LakeBase-only and were excluded. The relay is not part of this library.
