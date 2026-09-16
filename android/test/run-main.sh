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
remote_root=$("$adb" shell run-as "$pkg" pwd | tr -d '\r')
apk=$("$adb" shell pm path "$pkg" | tr -d '\r' | sed -n 's/^package://p' | head -n 1)
native="${apk%/base.apk}/lib/arm64"
code="$remote_root/no_backup/app-process-code"
database="$remote_root/no_backup/app-process-auto-db-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz"
standalone_stage=$(mktemp -d "$out/standalone-stage.XXXXXX")
mkdir -p "$standalone_stage/classes" "$standalone_stage/resources" "$out/standalone"
javac --release 8 -cp "$sdk/platforms/android-36.1/android.jar:$out/package/seekdb-java.jar:$out/package/mariadb-java-client-3.5.6.jar" \
  -d "$standalone_stage/classes" "$root/android/test/src/com/oceanbase/seekdb/test/"{Main,JdbcExample,AndroidSocketFactory}.java \
  "$root/java/test/com/oceanbase/seekdb/test/HybridScenario.java"
jar cf "$standalone_stage/test-main.jar" -C "$standalone_stage/classes" .
"$sdk/build-tools/36.1.0/d8" --min-api 28 --lib "$sdk/platforms/android-36.1/android.jar" \
  --output "$standalone_stage/seekdb-test-dex.jar" "$standalone_stage/test-main.jar" \
  "$out/package/seekdb-java.jar" "$out/package/mariadb-java-client-3.5.6.jar"
# D8 converts bytecode only; retain JDBC configuration and ServiceLoader metadata.
unzip -oq "$out/package/mariadb-java-client-3.5.6.jar" '*.properties' 'META-INF/services/*' \
  -d "$standalone_stage/resources"
jar uf "$standalone_stage/seekdb-test-dex.jar" -C "$standalone_stage/resources" .
mv "$standalone_stage/seekdb-test-dex.jar" "$out/standalone/seekdb-test-dex.jar"
"$adb" shell run-as "$pkg" mkdir -p "$code"
# Open the destination for writing, then make it read-only before writing DEX bytes.
"$adb" shell "run-as '$pkg' sh -c 'if [ -f $code/seekdb-test-dex.jar ]; then chmod 600 $code/seekdb-test-dex.jar; fi; exec 3>$code/seekdb-test-dex.jar; chmod 400 $code/seekdb-test-dex.jar; cat >&3'" \
  < "$out/standalone/seekdb-test-dex.jar"
"$adb" shell run-as "$pkg" env "CLASSPATH=$code/seekdb-test-dex.jar" \
  "LD_LIBRARY_PATH=$native" /system/bin/app_process "-Djava.library.path=$native" \
  /system/bin com.oceanbase.seekdb.test.Main "$database"
