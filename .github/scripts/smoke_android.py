#!/usr/bin/env python3
"""Build a test APK from the finished AAR and run it on an adb-connected Android."""

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import time
import urllib.request
import zipfile

import package
from smoke_packages import one, unpack, verify_jar_version


def run(arguments, **kwargs):
    try:
        return package.run(arguments, timeout=180, **kwargs)
    except subprocess.CalledProcessError as error:
        print((error.stdout or "") + (error.stderr or ""), flush=True)
        raise


def build_apk(work):
    sdk = Path(os.environ["ANDROID_HOME"])
    tools = sdk / "build-tools/35.0.0"
    platform = sdk / "platforms/android-30/android.jar"
    aar = one("*.aar")
    expected = aar.with_name(aar.name + ".sha256").read_text().split()
    if expected != [package.digest(aar), aar.name]:
        raise ValueError("Finished AAR checksum mismatch")
    payload = work / "aar"
    with zipfile.ZipFile(aar) as stream:
        stream.extractall(payload)
    metadata = json.loads((payload / "assets/seekdb/BUILD-INFO.json").read_text())
    verify_jar_version(payload / "classes.jar", metadata["package_versions"]["android"])
    # The JDBC client belongs to this consumer app, not the released AAR.
    jdbc = work / "mariadb-java-client-3.5.6.jar"
    urllib.request.urlretrieve("https://repo.maven.apache.org/maven2/org/mariadb/jdbc/"
                               "mariadb-java-client/3.5.6/" + jdbc.name, jdbc)
    classes = work / "classes"
    dex = work / "dex"
    classes.mkdir()
    dex.mkdir()
    sources = package.ROOT / "android/test/src/com/oceanbase/seekdb/test"
    run(["javac", "--release", "8", "-g:none", "-cp", os.pathsep.join(map(str,
         (platform, payload / "classes.jar", jdbc))), "-d", classes,
         *[sources / (name + ".java") for name in ("MainActivity", "JdbcExample", "AndroidSocketFactory")],
         package.ROOT / "java/test/com/oceanbase/seekdb/test/HybridScenario.java"])
    consumer = work / "consumer.jar"
    run(["jar", "cf", consumer, "-C", classes, "."])
    run([tools / "d8", "--min-api", "28", "--lib", platform, "--output", dex,
         consumer, payload / "classes.jar", jdbc])
    apk = work / "unsigned.apk"
    run([tools / "aapt2", "link", "-o", apk, "--manifest",
         package.ROOT / "android/test/AndroidManifest.xml", "-I", platform])
    with zipfile.ZipFile(apk, "a", zipfile.ZIP_STORED) as stream:
        for path in dex.glob("*.dex"):
            stream.write(path, path.name)
        for path in sorted((payload / "jni").rglob("*.so")):
            stream.write(path, "lib/" + path.relative_to(payload / "jni").as_posix())
        for path in sorted((payload / "assets").rglob("*")):
            if path.is_file():
                stream.write(path, "assets/" + path.relative_to(payload / "assets").as_posix())
    aligned = work / "aligned.apk"
    run([tools / "zipalign", "-f", "-P", "16", "4", apk, aligned])
    key = work / "debug.jks"
    run(["keytool", "-genkeypair", "-keystore", key, "-storepass", "android", "-keypass", "android",
         "-alias", "test", "-keyalg", "RSA", "-validity", "1", "-dname", "CN=seekdb smoke", "-noprompt"])
    signed = work / "smoke.apk"
    run([tools / "apksigner", "sign", "--ks", key, "--ks-pass", "pass:android", "--out", signed, aligned])
    run([tools / "apksigner", "verify", signed])
    return signed, metadata


def prepare(work):
    source = one("libseekdb-*.tar.gz")
    if source.with_name(source.name + ".sha256").read_text().split() != [package.digest(source), source.name]:
        raise ValueError("Finished C SDK checksum mismatch")
    sdk = unpack(source, work / "sdk")
    metadata = json.loads((sdk / "BUILD-INFO.json").read_text())
    apk = None
    if list(package.DIST.glob("*.aar")):
        apk, aar_metadata = build_apk(work)
        if any(aar_metadata[key] != metadata[key] for key in ("bindings_commit", "seekdb_commit", "package_versions")):
            raise ValueError("Finished SDK and AAR provenance mismatch")
    ndk = Path(os.environ["ANDROID_HOME"]) / "ndk" / package.NDK_VERSION
    compiler = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang"
    run([compiler, "-I" + str(sdk / "include"), Path(__file__).with_name("smoke") / "native.c",
         "-L" + str(sdk / "lib"), "-lseekdb", "-Wl,-rpath,$ORIGIN/lib", "-Wl,-z,max-page-size=16384",
         "-Wl,-z,common-page-size=16384", "-o", sdk / "client"])
    payload = work / "input"
    payload.mkdir()
    shutil.move(sdk, payload / "sdk")
    if apk:
        shutil.copy2(apk, payload / "smoke.apk")
    package.write_json(payload / "metadata.json", metadata)
    with tarfile.open(package.ROOT / "build/android-smoke-input.tar.gz", "w:gz", compresslevel=1) as stream:
        for path in payload.iterdir():
            stream.add(path, arcname=path.name)


def sdk_smoke(device, sdk):
    base = "/data/local/tmp/seekdb-package-smoke"
    device("shell", "rm", "-rf", base)
    device("push", sdk, base)
    device("shell", base + "/lib/seekdb", "-V")
    device("shell", base + "/client", base + "/database")
    result = device("shell", "-T", base + "/lib/seekdb_cli", base + "/cli-database",
                    input="SELECT 314159 AS smoke_value;\nquit\n", capture_output=True)
    if "314159" not in result.stdout or "ERROR:" in result.stderr:
        raise ValueError("Finished Android C SDK CLI smoke failed: " + result.stdout + result.stderr)
    device("shell", "rm", "-rf", base + "/database", base + "/cli-database")


def write_report(metadata, api, abi, components, status):
    report = package.DIST / (metadata["target"] + "-smoke.json")
    package.write_json(report, {"target": metadata["target"], "packages": components,
        "status": status, "runtime_api": int(api), "runtime_abi": abi,
        "source_commit": metadata["bindings_commit"],
        "test_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()})
    report.with_name(report.name + ".sha256").write_text(package.digest(report) + "  " + report.name + "\n")
    print(report.read_text(), flush=True)


@contextmanager
def runtime_report(metadata, api, abi):
    components = []
    write_report(metadata, api, abi, components, "running")
    try:
        yield components
    except Exception:
        write_report(metadata, api, abi, components, "failed")
        raise
    else:
        write_report(metadata, api, abi, components, "passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=package.DIST)
    parser.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL", "emulator-5554"))
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--prepared", type=Path)
    arguments = parser.parse_args()
    package.DIST = arguments.dist.resolve()
    work = package.WORK / "android-smoke"
    work.mkdir(parents=True)
    if arguments.prepare:
        prepare(work)
        return
    sdk = None
    if arguments.prepared:
        payload = unpack(arguments.prepared, work / "input")
        apk = payload / "smoke.apk" if (payload / "smoke.apk").is_file() else None
        metadata = json.loads((payload / "metadata.json").read_text())
        sdk = payload / "sdk"
    else:
        apk, metadata = build_apk(work)
    adb_binary = shutil.which("adb") or str(Path(os.environ["ANDROID_HOME"]) / "platform-tools/adb")
    adb = [adb_binary, "-s", arguments.serial]
    def device(*command, **kwargs):
        return run([*adb, *command], text=True, **kwargs)
    abi = device("shell", "getprop", "ro.product.cpu.abi", capture_output=True).stdout.strip()
    kernel_arch = device("shell", "uname", "-m", capture_output=True).stdout.strip()
    api = device("shell", "getprop", "ro.build.version.sdk", capture_output=True).stdout.strip()
    if abi != "arm64-v8a" or kernel_arch != "aarch64":
        raise ValueError("Test requires native arm64 Android; found " + abi + "/" + kernel_arch)
    with runtime_report(metadata, api, abi) as components:
        if sdk:
            sdk_smoke(device, sdk)
            components.append("sdk")
        if apk:
            device("install", "--no-incremental", "-r", apk)
            app = "com.oceanbase.seekdb.pr60"
            device("shell", "pm", "clear", app)
            device("logcat", "-c")
            passed = False
            try:
                # The installed APK can access only the final archive payload on Android.
                device("shell", "am", "start", "-W", "-n", app + "/com.oceanbase.seekdb.test.MainActivity")
                deadline = time.monotonic() + 300
                while True:
                    log = device("logcat", "-d", "-s", "SeekDBTest:I", "AndroidRuntime:E", capture_output=True).stdout
                    if "Test failed" in log or "FATAL EXCEPTION" in log:
                        raise RuntimeError("Android finished AAR smoke failed:\n" + log)
                    if all(marker in log for marker in ("OK transport=", "HYBRID_OK", "DIRECTORY_FD_CLOSED_OK")):
                        print(log, flush=True)
                        passed = True
                        break
                    if time.monotonic() >= deadline:
                        raise TimeoutError("Android finished AAR smoke timed out:\n" + log)
                    time.sleep(2)
            finally:
                (work / "logcat.log").write_text(device("logcat", "-d", capture_output=True).stdout)
                if not passed:
                    try:
                        installed = device("shell", "pm", "path", app, capture_output=True).stdout.strip().splitlines()[0]
                        native = installed.removeprefix("package:").rsplit("/", 1)[0] + "/lib/arm64"
                        diagnostic = subprocess.run([*adb, "shell", "run-as", app,
                            native + "/libseekdb_exec.so", "-V"], text=True, capture_output=True, timeout=30)
                        message = "exit=%s\n%s%s" % (diagnostic.returncode, diagnostic.stdout, diagnostic.stderr)
                        (work / "engine-version.log").write_text(message)
                        print("Engine execution diagnostic:\n" + message, flush=True)
                        print(device("shell", "run-as", app, "find", "no_backup", "-type", "f", capture_output=True).stdout)
                    except Exception as error:
                        print("Could not collect additional engine diagnostics: " + str(error), flush=True)
                device("shell", "am", "force-stop", app)
            components.append("android-aar")


if __name__ == "__main__":
    main()
