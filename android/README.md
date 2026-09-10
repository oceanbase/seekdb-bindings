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

Outputs are in `build/android/package`: two JARs, `jniLibs/arm64-v8a` and optionally
a signed test APK. The script pins native API 28 and NDK r27d, including the
sysroot, and defaults to four build jobs. Set BUILD_JOBS to adjust concurrency.

## Use

Include both JARs as app dependencies and copy all three native files to
`app/src/main/jniLibs/arm64-v8a`. Enable `android:extractNativeLibs="true"` and
Gradle legacy JNI packaging (`packaging { jniLibs { useLegacyPackaging = true } }`).
Preserve `com.oceanbase.seekdb.**` and `org.mariadb.jdbc.Driver` if enabling shrinking;
shrinking has not been tested. Keep the full database path plus `/run/sql.sock`
shorter than 108 bytes. Run operations off the UI thread.

```java
SeekDB.setBinaryPath(context.getApplicationInfo().nativeLibraryDir + "/libseekdb_exec.so");
String path = context.getNoBackupFilesDir().getAbsolutePath() + "/db";
try (EmbeddedSeekDB db = SeekDB.openUnixSocket(path);
     java.sql.Connection connection = db.connect("test")) {
    // connectionOptions() is obtained from libseekdb; use JDBC normally.
}
```

The C executable override is copied, serialized against other override reads/writes,
and affects subsequent opens. Android spawns use POSIX_SPAWN_USEVFORK to avoid the
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

Validated on the ARM64 Android 16 emulator (API 36, 36.1 image), native min API 28,
APK targetSdk 28. Newer targetSdk settings and physical devices are not verified.
The package is not a standalone pure-Java database; deploy the native files too.

Additional manual validation ran the original pyseekdb simple, complete, seven
hybrid scenarios, and sparse examples against the APK-owned engine through a
test-only ADB/Unix socket relay. Python and real embedding/reranking models ran on
macOS, including BM25 and CrossEncoder. These validate engine compatibility with
the SDK workloads, not Python/model execution inside Android. Namespace examples
are LakeBase-only and were excluded. The relay is not part of this library.
