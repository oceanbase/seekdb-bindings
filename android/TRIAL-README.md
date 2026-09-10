# SeekDB Android 试用包

## 内容与支持范围

- `aar/seekdb-android.aar`：公共 Java API、JNI、libseekdb、SeekDB 可执行文件。
- `examples/aar-consumer/`：独立 Gradle 示例，仅通过本地 AAR 引用 SeekDB。
- `demo/seekdb-test.apk`：示例构建的体验 APK；安装启动后自动执行查询及混合检索。
- `licenses/`：随包附带的许可证和第三方声明；构建版本与验证结果见测试报告。
- 仅支持 Android ARM64 (`arm64-v8a`)，原生最低 API 28。
- 这是试用版，不代表已通过所有 Android 版本、真机或生产负载认证。
- 示例 targetSdk 为 28；面向更高 targetSdk 和应用商店发布需另外验证。

## 集成到自己的项目

1. 将 AAR 放到应用模块的 `libs/`，添加 `implementation files('libs/seekdb-android.aar')`。
2. 不要同时引用 `seekdb-java.jar`，否则会出现重复类。
3. 配置 `android.packaging.jniLibs.useLegacyPackaging = true`，确保原生文件
   被提取到文件系统。不要关闭提取：SeekDB 需要通过实际路径启动子进程。
4. 使用 App 私有持久目录（例如 `context.getNoBackupFilesDir()` 的子目录），
   在后台线程调用以下 API。

```java
String path = new java.io.File(context.getNoBackupFilesDir(), "seekdb").getAbsolutePath();
try (com.oceanbase.seekdb.SeekDB db = com.oceanbase.seekdb.SeekDB.open(
        path, "mysql_port_mode", "disabled")) {
    com.oceanbase.seekdb.ConnectionOptions options = db.connectionOptions();
    // 选择自己的客户端驱动，用 options.unix_socket 等字段建立连接。
    // 先关闭客户端连接，再关闭 db。
}
```

无需设置二进制路径。libseekdb 在自身目录查找 `seekdb`，不存在时回退到
`libseekdb_exec.so`。后者是为 APK 提取机制命名的可执行文件，不是动态库。
JNI 与 libseekdb 本身才是动态库。AAR 不包含 JDBC 驱动或测试代码。

## 构建与运行示例

安装 SDK platform 36.1、Build Tools 36.1.0，以及 JDK 17 或兼容版本。
示例固定 Android Gradle Plugin 8.13.2 和 Gradle 8.13。
设置 `ANDROID_HOME` 为自己的 SDK 目录（或在示例 local.properties 中设置 sdk.dir）。

```sh
cd examples/aar-consumer
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.oceanbase.seekdb.trial/com.oceanbase.seekdb.test.MainActivity
adb logcat -s SeekDBTest:I
```

预期出现 `OK transport=unix_socket port=0 value=1`、`HYBRID_OK`、
`DIRECTORY_FD_CLOSED_OK`。数据库保存在试用 App 私有目录，不会使用 /tmp。
测试会重建它自己的 `android_hybrid_demo` 表；不要指向包含重要数据的实例。

示例使用 MariaDB JDBC 3.5.6，并附带其 JAR。`JdbcExample` 和
`AndroidSocketFactory` 是该驱动的示例配置/适配代码，不是通用 bindings API。
其他驱动应采用自己的 Unix socket 连接方案。反射加载的适配器在开启混淆时
需要保留类名；本试用示例不启用混淆。AAR 自带公共 JNI API 的保留规则。

## 注意事项

初次启动需要初始化数据库，可能等待较长时间，且会占用数百 MB 存储空间。
请勿在 UI 线程启动。关闭 handle 不保证服务进程立即退出，也不删除数据库。
`/proc/self/fd/` socket 地址只在对应进程和 handle 生命周期内有效。
不足的设备空间可能导致安装失败；不要通过删除个人数据来解决测试空间问题。
