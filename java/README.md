# SeekDB Java bindings

`src/main/java` contains the platform-neutral API (`SeekDB`, `EmbeddedSeekDB`,
`ConnectionOptions`). `jni/seekdb_jni.c` is the common JNI bridge and has no
Android-specific includes. Build the Java API without the Android SDK:

```sh
bash java/build.sh
```

The output is `build/java/seekdb-java.jar`. This is not a pure-Java database:
each target still needs a matching libseekdb, JNI library and SeekDB executable.
Desktop native packaging and Unix/named-pipe JDBC adapters are not yet provided
or runtime-verified by this Android work.

The only open entry point is `SeekDB.open(String dbDir, String... parameters)`.
Parameters are optional key/value pairs, passed unchanged to libseekdb:
`SeekDB.open(path, "mysql_port_mode", "disabled", "port", "0")`.
Omitting parameters uses native defaults. Java validates the pair structure only;
there is no separate port argument or transport-specific open method.

Connection options preserve the C API fields exactly: `transport`, `port`,
`host`, `unix_socket`, `named_pipe`, `user`. Unused strings remain null. No
combined `endpoint` field is used. TCP JDBC URLs use the returned host; Unix
socket JDBC connections require an explicitly supplied platform socket factory.
Named-pipe options are exposed unchanged for clients/adapters to consume; the
built-in JDBC helper currently reports that no named-pipe adapter is configured.

Android's LocalSocket adapter, NDK build and device tests remain in `android/`.
An Android app includes `seekdb-java.jar`, `seekdb-android.jar` (adapter only),
and the MariaDB JDBC JAR. See [Android instructions](../android/README.md).

## Open errors

`seekdb_last_error(connection, ...)` reports SQL connection errors and requires
a connection. For open failures, C callers can immediately read
`seekdb_last_open_error()` on the same thread. Its borrowed string is overwritten
by the next `seekdb_open` on that thread; successful open clears it. Java's
`nativeOpen` includes both the return code and this description in its exception.
Diagnostics include argument validation, directory errors, spawn failures, and
startup stages/timeouts. A child-process startup failure may still require the
SeekDB instance logs for its internal root cause.
