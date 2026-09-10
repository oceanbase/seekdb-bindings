#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${JAVA_BUILD_DIR:-"$root/build/java"}
mkdir -p "$out/classes"
javac --release 8 -d "$out/classes" "$root"/java/src/main/java/com/oceanbase/seekdb/*.java
jar cf "$out/seekdb-java.jar" -C "$out/classes" .
javac --release 8 -cp "$out/seekdb-java.jar" -d "$out/tests" "$root/java/test/ConnectionOptionsTest.java"
java -cp "$out/seekdb-java.jar:$out/tests" ConnectionOptionsTest
