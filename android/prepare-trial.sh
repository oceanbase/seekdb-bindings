#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${ANDROID_BUILD_DIR:-"$root/build/android"}
# Use a new destination to avoid accidentally shipping stale files or user data.
trial=${TRIAL_DIR:?Set TRIAL_DIR to a new output directory}
if [[ -e "$trial" ]]; then
  echo "TRIAL_DIR already exists; choose a new directory" >&2
  exit 1
fi
test -f "$out/package/seekdb-android.aar"
test -f "$out/package/mariadb-java-client-3.5.6.jar"
mkdir -p "$trial/aar" "$trial/examples" "$trial/demo"
cp "$out/package/seekdb-android.aar" "$trial/aar/"
cp -R "$root/android/examples/aar-consumer" "$trial/examples/"
project="$trial/examples/aar-consumer"
mkdir -p "$project/app/libs" "$project/app/src/main/java/com/oceanbase/seekdb/test"
cp "$out/package/seekdb-android.aar" "$out/package/mariadb-java-client-3.5.6.jar" "$project/app/libs/"
cp "$root"/android/test/src/com/oceanbase/seekdb/test/{MainActivity,JdbcExample,AndroidSocketFactory}.java \
   "$root/java/test/com/oceanbase/seekdb/test/HybridScenario.java" \
   "$project/app/src/main/java/com/oceanbase/seekdb/test/"
cp "$root/android/TRIAL-README.md" "$trial/README.md"
echo "Prepared $trial; build examples/aar-consumer with Gradle 8.13 and test its APK."
echo "Only after validation, copy that APK to demo/seekdb-test.apk."
