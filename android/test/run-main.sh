#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
sdk=${ANDROID_SDK_ROOT:?Set ANDROID_SDK_ROOT}
adb="$sdk/platform-tools/adb"
out=${ANDROID_BUILD_DIR:-"$root/build/android"}
pkg=${ANDROID_TEST_PACKAGE:-com.oceanbase.seekdb.pr60}
if [[ ! "$pkg" =~ ^[A-Za-z0-9_.]+$ ]]; then
  echo "Invalid Android package name" >&2
  exit 1
fi
# Borrow an existing debuggable app's identity/storage; do not build or launch an APK.
remote_root=$($adb shell run-as "$pkg" pwd | tr -d '\r')
apk=$($adb shell pm path "$pkg" | tr -d '\r' | sed -n 's/^package://p' | head -n 1)
native="${apk%/base.apk}/lib/arm64"
code="$remote_root/no_backup/app-process-code"
database="$remote_root/no_backup/app-process-db-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz"
mkdir -p "$out/standalone/classes"
javac --release 8 -cp "$sdk/platforms/android-36.1/android.jar:$out/package/seekdb-java.jar:$out/package/seekdb-android.jar" \
  -d "$out/standalone/classes" "$root/android/test/src/com/oceanbase/seekdb/test/"{Main,JdbcExample}.java \
  "$root/java/test/com/oceanbase/seekdb/test/HybridScenario.java"
jar cf "$out/standalone/test-main.jar" -C "$out/standalone/classes" .
"$sdk/build-tools/36.1.0/d8" --min-api 28 --lib "$sdk/platforms/android-36.1/android.jar" \
  --output "$out/standalone/seekdb-test-dex.jar" "$out/standalone/test-main.jar" \
  "$out/package/seekdb-java.jar" "$out/package/seekdb-android.jar" "$out/package/mariadb-java-client-3.5.6.jar"
# D8 converts bytecode only; retain JDBC configuration and ServiceLoader metadata.
mkdir -p "$out/standalone/resources"
unzip -oq "$out/package/mariadb-java-client-3.5.6.jar" '*.properties' 'META-INF/services/*' \
  -d "$out/standalone/resources"
jar uf "$out/standalone/seekdb-test-dex.jar" -C "$out/standalone/resources" .
"$adb" shell run-as "$pkg" mkdir -p "$code"
# Open the destination for writing, then make it read-only before writing DEX bytes.
"$adb" shell "run-as '$pkg' sh -c 'if [ -f $code/seekdb-test-dex.jar ]; then chmod 600 $code/seekdb-test-dex.jar; fi; exec 3>$code/seekdb-test-dex.jar; chmod 400 $code/seekdb-test-dex.jar; cat >&3'" \
  < "$out/standalone/seekdb-test-dex.jar"
"$adb" shell run-as "$pkg" env "CLASSPATH=$code/seekdb-test-dex.jar" \
  "LD_LIBRARY_PATH=$native" /system/bin/app_process "-Djava.library.path=$native" \
  /system/bin com.oceanbase.seekdb.test.Main "$native/libseekdb_exec.so" "$database"
