#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${ANDROID_SDK_ROOT:?Set ANDROID_SDK_ROOT}
ndk="$sdk/ndk/27.3.13750724"
bt="$sdk/build-tools/36.1.0"
platform="$sdk/platforms/android-36.1/android.jar"
tc="$ndk/toolchains/llvm/prebuilt/$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64/bin"
out=${ANDROID_BUILD_DIR:-"$root/build/android"}
openssl=${ANDROID_OPENSSL_ROOT:?Set ANDROID_OPENSSL_ROOT to an arm64 API28 static OpenSSL installation}
engine=${SEEKDB_ANDROID_BIN:?Set SEEKDB_ANDROID_BIN to the Android ARM64 SeekDB executable}
jdbc=${MARIADB_JDBC_JAR:?Set MARIADB_JDBC_JAR to mariadb-java-client-3.5.6.jar}
mkdir -p "$out/package/jniLibs/arm64-v8a"
# Keep a previous adapter artifact out of the package without deleting it.
if [[ -f "$out/package/seekdb-android.jar" ]]; then
  retired=$(mktemp -d "$out/retired.XXXXXX")
  mv "$out/package/seekdb-android.jar" "$retired/"
fi
env -u CC -u CXX -u SDKROOT ANDROID_NDK_HOME="$ndk" cmake -S "$root" -B "$out/native" \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DSEEKDB_BUILD_PYTHON=OFF \
  -DOPENSSL_USE_STATIC_LIBS=TRUE -DOPENSSL_ROOT_DIR="$openssl" \
  -DOPENSSL_INCLUDE_DIR="$openssl/include" \
  -DOPENSSL_SSL_LIBRARY="$openssl/lib/libssl.a" \
  -DOPENSSL_CRYPTO_LIBRARY="$openssl/lib/libcrypto.a"
cmake --build "$out/native" --target seekdb -j "${BUILD_JOBS:-4}"
cp "$out/native/libseekdb.so" "$out/package/jniLibs/arm64-v8a/"
"$tc/aarch64-linux-android28-clang" -shared -fPIC -I"$root/lib/include" \
  "$root/java/jni/seekdb_jni.c" -L"$out/native" -lseekdb \
  -Wl,--no-undefined -o "$out/package/jniLibs/arm64-v8a/libseekdb_jni.so"
"$tc/llvm-strip" --strip-debug "$engine" -o "$out/package/jniLibs/arm64-v8a/libseekdb_exec.so"
java_classes=$(mktemp -d "$out/java-classes.XXXXXX")
javac --release 8 -d "$java_classes" \
  "$root"/java/src/main/java/com/oceanbase/seekdb/*.java
jar cf "$out/package/seekdb-java.jar" -C "$java_classes" .
cp "$jdbc" "$out/package/mariadb-java-client-3.5.6.jar"
if [[ ${BUILD_TEST_APK:-0} == 1 ]]; then
  mkdir -p "$out/test/classes" "$out/test/dex" "$out/test/lib/arm64-v8a"
  "$bt/aapt2" link -o "$out/test/unsigned.apk" --manifest "$root/android/test/AndroidManifest.xml" -I "$platform"
  javac --release 8 -cp "$platform:$out/package/seekdb-java.jar:$jdbc" -d "$out/test/classes" \
    "$root"/android/test/src/com/oceanbase/seekdb/test/{MainActivity,JdbcExample,AndroidSocketFactory}.java \
    "$root/java/test/com/oceanbase/seekdb/test/HybridScenario.java"
  jar cf "$out/test/test-classes.jar" -C "$out/test/classes" .
  "$bt/d8" --min-api 28 --lib "$platform" --output "$out/test/dex" \
    "$out/test/test-classes.jar" "$out/package/seekdb-java.jar" "$jdbc"
  cp "$out/test/dex/classes.dex" "$out/test/classes.dex"
  cp "$out/package/jniLibs/arm64-v8a/"*.so "$out/test/lib/arm64-v8a/"
  (cd "$out/test" && zip -q -0 unsigned.apk classes.dex lib/arm64-v8a/*.so)
  "$bt/zipalign" -f -P 16 4 "$out/test/unsigned.apk" "$out/test/aligned.apk"
  if [[ ! -f "$out/test/debug.jks" ]]; then
    keytool -genkeypair -keystore "$out/test/debug.jks" -storepass android -keypass android \
      -alias test -keyalg RSA -validity 3650 -dname 'CN=SeekDB Android Test' -noprompt
  fi
  "$bt/apksigner" sign --ks "$out/test/debug.jks" --ks-pass pass:android \
    --out "$out/package/seekdb-hybrid-test.apk" "$out/test/aligned.apk"
  "$bt/apksigner" verify "$out/package/seekdb-hybrid-test.apk"
fi
