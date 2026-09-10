#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${JAVA_BUILD_DIR:-"$root/build/java"}
mkdir -p "$out"
# Compile in a fresh directory so removed API classes cannot leak into the JAR.
classes=$(mktemp -d "$out/classes.XXXXXX")
javac --release 8 -d "$classes" "$root"/java/src/main/java/com/oceanbase/seekdb/*.java
jar cf "$out/seekdb-java.jar" -C "$classes" .
javac --release 8 -cp "$out/seekdb-java.jar" -d "$out/tests" \
  "$root/java/test/ConnectionOptionsTest.java" \
  "$root/java/test/com/oceanbase/seekdb/test/HybridScenario.java"
java -cp "$out/seekdb-java.jar:$out/tests" ConnectionOptionsTest
