#!/usr/bin/env python3
"""Package bindings around a verified seekdb Actions binary, without building the engine."""

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import inspect
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "build/package"
DIST = ROOT / "dist"
# The merged binary-distribution protocol; sparse-checkout only its packaging tools.
PACKAGING_REF = "8084119a13832873c5ef68732f556d2a924e10ea"
NDK_VERSION = "27.3.13750724"
VERSION_COMPONENTS = ("sdk", "python", "node", "java", "android")
VERSION_ENV = ("PACKAGE_VERSION", *["PACKAGE_" + name.upper() + "_VERSION" for name in VERSION_COMPONENTS])
TARGETS = [
    dict(id="linux-x86_64", os="linux", arch="x86_64", runner="ubuntu-24.04"),
    dict(id="linux-aarch64", os="linux", arch="aarch64", runner="ubuntu-24.04-arm"),
    dict(id="macos-arm64", os="macos", arch="arm64", runner="macos-15"),
    dict(id="windows-x86_64", os="windows", arch="x86_64", runner="windows-2022"),
    dict(id="android-arm64", os="android", arch="aarch64", runner="ubuntu-24.04"),
]


def run(arguments, **kwargs):
    arguments = [str(value) for value in arguments]
    if arguments[0] == "npm":
        arguments = npm_command(*arguments[1:])
    print("+ " + subprocess.list2cmdline(arguments), flush=True)
    return subprocess.run(arguments, check=True, **kwargs)


def npm_command(*arguments):
    node = Path(shutil.which("node")).resolve()
    candidates = [node.parent / "node_modules/npm/bin/npm-cli.js",
                  node.parent.parent / "lib/node_modules/npm/bin/npm-cli.js"]
    installed = shutil.which("npm")
    if installed and Path(installed).resolve().suffix in (".js", ".mjs"):
        candidates.append(Path(installed).resolve())
    cli = next(path for path in candidates if path.is_file())
    return [str(node), str(cli), *[str(value) for value in arguments]]


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def output(name, value):
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as stream:
            stream.write(name + "=" + value + "\n")
    print(name + "=" + value)


def api(path):
    if not os.environ.get("GITHUB_TOKEN"):
        return json.loads(subprocess.check_output(["gh", "api", path], text=True))
    request = urllib.request.Request("https://api.github.com/" + path,
        headers={"Accept": "application/vnd.github+json", "Authorization": "Bearer " + os.environ["GITHUB_TOKEN"]})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def dependency_key():
    key = subprocess.check_output(["git", "ls-tree", "HEAD", "deps/"], cwd=ROOT)
    for path in (".github/scripts/package.py", "scripts/docker/bindings-manylinux.Dockerfile",
                 "python/pyproject.toml", "typescript/package-lock.json"):
        key += (ROOT / path).read_bytes()
    return hashlib.sha256(key).hexdigest()


def matrix():
    repository = os.environ.get("BINARY_REPOSITORY", "oceanbase/seekdb")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("BINARY_REPOSITORY must be owner/repository")
    run_id = str(int(os.environ["BINARY_RUN_ID"]))
    source = api("repos/%s/actions/runs/%s" % (repository, run_id))
    if source["status"] != "completed":
        raise ValueError("Choose a completed Package seekdb workflow run")
    artifacts = []
    page = 1
    while True:
        batch = api("repos/%s/actions/runs/%s/artifacts?per_page=100&page=%s" % (repository, run_id, page))["artifacts"]
        artifacts.extend(item["name"] for item in batch if not item["expired"])
        if len(batch) < 100:
            break
        page += 1
    selected = os.environ.get("PACKAGE_PLATFORM", "all")
    component = os.environ.get("PACKAGE_COMPONENT", "all")
    baseline = os.environ.get("BINARY_GLIBC", "2.28")
    if component not in ("all", "sdk", "python", "node", "java") or baseline not in ("2.17", "2.28"):
        raise ValueError("Unsupported binding component or Linux binary baseline")
    targets = [dict(item) for item in TARGETS if selected in ("all", item["id"])]
    if not targets:
        raise ValueError("Unsupported platform: " + selected)
    if component in ("python", "node"):
        targets = [item for item in targets if item["os"] != "android"]
        if not targets:
            raise ValueError(component + " does not target Android")
    for item in targets:
        binary = {
            "linux": "linux-%s-glibc%s" % (item["arch"], baseline),
            "macos": "macos15-arm64", "windows": "windows-x86_64-md", "android": "android-arm64-api26",
        }[item["os"]]
        item["artifact"] = "binary-" + binary
        if item["artifact"] not in artifacts:
            raise ValueError("Run %s has no unexpired %s; select another run or an individual platform" % (run_id, item["artifact"]))
    output("matrix", json.dumps({"include": targets}, separators=(",", ":")))
    output("source_commit", source["head_sha"])
    output("packaging_ref", PACKAGING_REF)
    output("timestamp", datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    output("dependency_key", dependency_key())


def packaging_tools():
    directory = Path(os.environ.get("SEEKDB_PACKAGING_DIR", ROOT / "_seekdb/package")).resolve()
    sys.path.insert(0, str(directory))
    import binary_distribution
    import debug_symbols
    return binary_distribution, debug_symbols


def selected(name):
    return os.environ.get("PACKAGE_COMPONENT", "all") in ("all", name)


def package_versions(engine_version):
    import tomllib

    defaults = dict.fromkeys(VERSION_COMPONENTS, engine_version)
    defaults["python"] = tomllib.loads((ROOT / "python/pyproject.toml").read_text())["project"]["version"]
    defaults["node"] = json.loads((ROOT / "typescript/package.json").read_text())["version"]
    common = os.environ.get("PACKAGE_VERSION", "").strip()
    versions = {name: os.environ.get("PACKAGE_" + name.upper() + "_VERSION", "").strip() or common or defaults[name]
                for name in VERSION_COMPONENTS}
    for name, value in versions.items():
        if not re.fullmatch(r"[0-9][0-9A-Za-z.+_-]*", value):
            raise ValueError("Invalid %s package version: %r" % (name, value))
    if os.environ["PACKAGE_OS"] != "android":
        if selected("python"):
            from packaging.version import Version
            versions["python"] = str(Version(versions["python"]))
        if selected("node"):
            # Use npm's own SemVer parser, including its prerelease rules.
            npm = Path(npm_command()[1]).parent.parent
            script = "const v=require(process.argv[1]).valid(process.argv[2]);if(!v)throw Error('Invalid npm package version: '+process.argv[2]);process.stdout.write(v)"
            versions["node"] = subprocess.check_output([shutil.which("node"), "-e", script,
                npm / "node_modules/semver", versions["node"]], text=True)
    print("Package versions: " + json.dumps(versions, sort_keys=True), flush=True)
    return versions


@contextmanager
def python_project_version(version):
    project = ROOT / "python/pyproject.toml"
    original = project.read_bytes()
    updated, count = re.subn(r'(?m)^version\s*=\s*"[^"\r\n]*"',
                            "version = " + json.dumps(version), original.decode("utf-8"), count=1)
    if count != 1:
        raise ValueError("Missing static Python project version")
    try:
        project.write_bytes(updated.encode("utf-8"))
        yield
    finally:
        project.write_bytes(original)


def executable_name(name):
    return name + (".exe" if os.environ["PACKAGE_OS"] == "windows" else "")


def library_name():
    return {"linux": "libseekdb.so", "macos": "libseekdb.dylib",
            "windows": "seekdb.dll", "android": "libseekdb.so"}[os.environ["PACKAGE_OS"]]


def build_output(directory, name):
    return next(path for path in (directory / name, directory / "Release" / name) if path.is_file())


def archive(prefix, kind, metadata, component="sdk"):
    identity = metadata["target"]
    name = "%s-%s-%s-%s-sdb%s-bnd%s.tar.gz" % (kind, metadata["package_versions"][component], identity,
        metadata["built_at"], metadata["seekdb_commit"][:12], metadata["bindings_commit"][:12])
    write_json(prefix / "BUILD-INFO.json", metadata)
    checksums = "".join("%s  %s\n" % (digest(path), path.relative_to(prefix).as_posix())
        for path in sorted(prefix.rglob("*")) if path.is_file() and path != prefix / "SHA256SUMS")
    (prefix / "SHA256SUMS").write_text(checksums, encoding="utf-8")
    destination = DIST / name
    def owner(member):
        member.uid = member.gid = 0
        member.uname = member.gname = "root"
        return member
    with tarfile.open(destination, "w:gz", compresslevel=1) as stream:
        for path in sorted(prefix.iterdir()):
            stream.add(path, arcname=path.name, filter=owner)
    return destination


def runtime():
    bd, symbols = packaging_tools()
    inputs = Path(os.environ.get("BINARY_INPUT", ROOT / "input"))
    archives = [p for p in inputs.glob("seekdb-binary-*") if p.name.endswith((".tar.gz", ".zip"))]
    if len(archives) != 1:
        raise ValueError("Expected one seekdb-binary archive in " + str(inputs))
    source = archives[0]
    checksum = source.with_name(source.name + ".sha256").read_text().split()
    if checksum != [digest(source), source.name]:
        raise ValueError("Binary archive checksum mismatch")
    prefix, manifest = bd.load(source, WORK / "engine")
    target = manifest["platform"]
    if target["os"] != os.environ["PACKAGE_OS"] or target["arch"] != os.environ["PACKAGE_ARCH"]:
        raise ValueError("Binary platform does not match this job")
    if manifest["source_commit"] != os.environ["BINARY_SOURCE_COMMIT"]:
        raise ValueError("Binary source commit does not match the selected workflow run")
    if target["os"] == "linux" and bd.version_tuple(target["glibc"]) > bd.version_tuple("2.28"):
        raise ValueError("Bindings require an engine compatible with glibc 2.28")
    if target["os"] in ("linux", "android"):
        try:
            for path, kind in symbols.native_files(prefix):
                if kind == "elf":
                    bd.elf_relocation_page_size(path)
        except ValueError as error:
            raise ValueError("Invalid ELF layout in %s; regenerate this input with the current seekdb packaging workflow: %s" %
                             (source.name, error)) from error
    return prefix, manifest, {"seekdb_archive": source.name, "seekdb_archive_sha256": digest(source),
        "seekdb_commit": manifest["source_commit"], "engine_version": manifest["version"],
        "engine_platform": target, "bindings_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "built_at": os.environ["PACKAGE_TIMESTAMP"], "binary_repository": os.environ.get("BINARY_REPOSITORY", "oceanbase/seekdb"),
        "binary_run_id": os.environ.get("BINARY_RUN_ID", "")}


def repair_native(directory):
    bd, symbols = packaging_tools()
    layout = os.environ["PACKAGE_OS"]
    for path, kind in symbols.native_files(directory):
        if layout in ("linux", "android") and path.name == "seekdb":
            current = bd.dynamic(path) if layout == "linux" else subprocess.check_output(
                [symbols.tool("readelf", layout), "-dW", str(path)], text=True)
            if not re.search(r"\((?:RUNPATH|RPATH)\).*?\[\$ORIGIN\]", current):
                run(["patchelf", "--page-size", str(bd.elf_relocation_page_size(path)),
                     "--set-rpath", "$ORIGIN", path])
        if layout == "macos":
            commands = subprocess.check_output(["otool", "-l", str(path)], text=True)
            if not re.search(r"cmd LC_RPATH\s+cmdsize \d+\s+path @loader_path \(", commands):
                run(["install_name_tool", "-add_rpath", "@loader_path", path])
            imports = subprocess.check_output(["otool", "-L", str(path)], text=True)
            for line in imports.splitlines()[1:]:
                dependency = line.strip().split(" (compatibility version", 1)[0]
                if dependency.startswith("@executable_path/../lib/seekdb/"):
                    run(["install_name_tool", "-change", dependency,
                         "@loader_path/" + dependency.rsplit("/", 1)[1], path])
        symbols.strip_file(path, kind, layout)
        if layout == "macos":
            run(["codesign", "--force", "--sign", "-", path])
    symbols.verify_stripped(directory)
    if layout == "macos":
        script = "import {verifyStagedMacOSRuntime} from " + json.dumps(
            (ROOT / "typescript/scripts/macos-runtime.mjs").as_uri()) + ";verifyStagedMacOSRuntime(process.argv[1]);"
        run(["node", "--input-type=module", "-e", script, directory])
    if layout == "linux":
        required = bd.glibc_requirement([path for path, kind in symbols.native_files(directory) if kind == "elf"])
        if bd.version_tuple(required) > bd.version_tuple("2.28"):
            raise ValueError("Binding payload requires GLIBC_" + required)
    elif layout == "android":
        for path, _ in symbols.native_files(directory):
            headers = subprocess.check_output([symbols.tool("readelf", layout), "-lW", str(path)], text=True)
            alignments = re.findall(r"^\s*LOAD\b.*\s(0x[0-9a-fA-F]+)\s*$", headers, re.MULTILINE)
            if not alignments or any(int(value, 16) < 16384 for value in alignments):
                raise ValueError("Android runtime requires 16 KiB LOAD alignment: " + str(path))
            bd.elf_relocation_page_size(path)


def openssl():
    layout = os.environ["PACKAGE_OS"]
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT / "deps/openssl", text=True).strip()
    recipe = hashlib.sha256((inspect.getsource(openssl) + NDK_VERSION).encode()).hexdigest()[:12]
    prefix = ROOT / "deps/.cache/openssl" / (os.environ["PACKAGE_TARGET"] + "-" + revision + "-" + recipe)
    if all((prefix / path).is_file() for path in ("lib/libssl.a", "lib/libcrypto.a", "include/openssl/ssl.h")):
        return prefix
    build = WORK / "openssl"
    build.mkdir()
    environment = dict(os.environ)
    target = {"linux": "linux-" + os.environ["PACKAGE_ARCH"], "macos": "darwin64-arm64-cc",
              "android": "android-arm64"}[layout]
    options = []
    if layout == "android":
        environment["ANDROID_NDK_ROOT"] = environment["ANDROID_NDK_HOME"]
        environment["PATH"] = str(Path(environment["ANDROID_NDK_HOME"]) / "toolchains/llvm/prebuilt/linux-x86_64/bin") + os.pathsep + environment["PATH"]
        options += ["-D__ANDROID_API__=28"]
    elif layout == "macos":
        options += ["-mmacosx-version-min=15.0"]
    run(["perl", ROOT / "deps/openssl/Configure", target, "no-shared", "no-tests", "no-apps",
         "--prefix=" + str(prefix), "--libdir=lib", "-fPIC", *options], cwd=build, env=environment)
    run(["make", "-j" + str(int(os.environ.get("BUILD_JOBS", "0")) or os.cpu_count())], cwd=build, env=environment)
    run(["make", "install_sw"], cwd=build, env=environment)
    return prefix


def stage_engine(engine, prefix):
    native = prefix / "lib"
    native.mkdir(parents=True, exist_ok=True)
    for source in (engine / "bin", engine / "lib/seekdb"):
        for path in source.iterdir():
            if path.is_file():
                shutil.copy2(path, native / path.name)
    for name in ("etc/seekdb", "share/seekdb/admin", "share/seekdb/srs", "share/seekdb/dependencies", "licenses"):
        if (engine / name).is_dir():
            shutil.copytree(engine / name, prefix / name, dirs_exist_ok=True)
    srs_helper = engine / "libexec/seekdb/import_srs_data.py"
    if srs_helper.is_file():
        destination = prefix / "libexec/seekdb/import_srs_data.py"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(srs_helper, destination)


def sdk(engine, metadata):
    layout = os.environ["PACKAGE_OS"]
    metadata["tls_provider"] = "schannel" if layout == "windows" else "openssl"
    if layout != "windows":
        version = dict(re.findall(r"^(MAJOR|MINOR|PATCH)=(\d+)$", (ROOT / "deps/openssl/VERSION.dat").read_text(), re.MULTILINE))
        metadata["openssl_version"] = ".".join(version[name] for name in ("MAJOR", "MINOR", "PATCH"))
        metadata["openssl_commit"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT / "deps/openssl", text=True).strip()
    build = ROOT / "build/node-sdk"
    prefix = WORK / "sdk"
    native = prefix / "lib"
    native.mkdir(parents=True)
    arguments = ["cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
        "-DSEEKDB_BUILD_PYTHON=OFF", "-DSEEKDB_BUILD_JAVA=" + ("ON" if selected("java") else "OFF"),
        "-DSEEKDB_BIN=" + str(engine / "bin" / executable_name("seekdb"))]
    if layout == "windows":
        arguments += ["-G", "Visual Studio 17 2022", "-A", "x64", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL", "-DWITH_SSL=SCHANNEL"]
    else:
        ssl = openssl()
        arguments += ["-DWITH_SSL=OPENSSL", "-DOPENSSL_USE_STATIC_LIBS=TRUE", "-DOPENSSL_ROOT_DIR=" + str(ssl),
            "-DOPENSSL_INCLUDE_DIR=" + str(ssl / "include"), "-DOPENSSL_SSL_LIBRARY=" + str(ssl / "lib/libssl.a"),
            "-DOPENSSL_CRYPTO_LIBRARY=" + str(ssl / "lib/libcrypto.a")]
    if layout == "macos":
        arguments += ["-DWITH_EXTERNAL_ZLIB=YES", "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0"]
    elif layout == "android":
        arguments += ["-DCMAKE_TOOLCHAIN_FILE=" + os.environ["ANDROID_NDK_HOME"] + "/build/cmake/android.toolchain.cmake",
            "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-28", "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"]
    run(arguments)
    targets = ["seekdb", "seekdb_cli"] + (["seekdb_jni"] if selected("java") else [])
    run(["cmake", "--build", build, "--config", "Release", "--parallel", str(int(os.environ.get("BUILD_JOBS", "0")) or os.cpu_count()),
         "--target", *targets])
    stage_engine(engine, prefix)
    names = [library_name(), executable_name("seekdb_cli")]
    if selected("java"):
        names.append({"windows": "seekdb_jni.dll", "macos": "libseekdb_jni.dylib"}.get(layout, "libseekdb_jni.so"))
    if layout == "windows":
        names.append("seekdb.lib")
    sources = [build_output(build, name) for name in names]
    if layout == "linux":
        # Collect only newly built bindings here; upstream engine DSOs are already bundled.
        extra = WORK / "binding-runtime"
        extra.mkdir()
        for path in sources:
            shutil.copy2(path, extra / path.name)
        script = "import {bundleLinuxRuntime} from " + json.dumps(
            (ROOT / "typescript/scripts/linux-runtime.mjs").as_uri()) + ";"
        script += "bundleLinuxRuntime(process.argv.slice(1)," + json.dumps(str(extra)) + ");"
        run(["node", "--input-type=module", "-e", script, *sources])
        sources = sorted(extra.iterdir())
    for path in sources:
        destination = native / path.name
        if destination.exists() and digest(destination) != digest(path):
            raise ValueError("Engine and binding libraries conflict: " + path.name)
        shutil.copy2(path, destination)
    shutil.copytree(ROOT / "lib/include", prefix / "include")
    licenses = prefix / "licenses/bindings"
    licenses.mkdir(parents=True)
    shutil.copy2(ROOT / "LICENSE", licenses / "LICENSE")
    shutil.copy2(ROOT / "deps/mariadb-connector-c/COPYING.LIB", licenses / "MariaDB-COPYING.LIB")
    shutil.copy2(ROOT / "deps/mariadb-connector-c/external/zlib/zlib.h", licenses / "zlib.h")
    if layout != "windows":
        shutil.copy2(ROOT / "deps/openssl/LICENSE.txt", licenses / "OpenSSL-LICENSE.txt")
    metadata["target"] = os.environ["PACKAGE_TARGET"] + {
        "linux": "-glibc2.28", "macos": "-min15.0", "windows": "-md", "android": "-api28",
    }[layout]
    repair_native(native)
    if layout != "android":
        run([native / executable_name("seekdb"), "-V"])
        code = "import ctypes,os,sys;" + ("handle=os.add_dll_directory(sys.argv[1]);" if layout == "windows" else "")
        run([sys.executable, "-c", code + "ctypes.CDLL(sys.argv[2])", native, native / library_name()])
    archive(prefix, "libseekdb", metadata)
    return prefix


def python_wheels(prefix, metadata):
    with python_project_version(metadata["package_versions"]["python"]):
        build_python_wheels(prefix)


def build_python_wheels(prefix):
    layout = os.environ["PACKAGE_OS"]
    for abi in ("311", "312"):
        interpreter = os.environ["PYTHON_" + abi]
        venv = WORK / ("python-" + abi)
        run([interpreter, "-m", "venv", venv])
        python = venv / ("Scripts/python.exe" if layout == "windows" else "bin/python")
        repair = {"linux": "auditwheel", "macos": "delocate", "windows": "delvewheel"}[layout]
        run([python, "-m", "pip", "install", "build", "nanobind>=2", "scikit-build-core>=0.10",
             "cmake>=3.20,<4", "ninja", repair, "PyMySQL>=1.1", "aiomysql"])
        if abi == "312":
            run([python, "-m", "pip", "install", "abi3audit"])
        raw = WORK / ("wheel-" + abi)
        arguments = [python, "-m", "build", "--wheel", "--no-isolation", "--outdir", raw,
                     "-Ccmake.define.SEEKDB_SDK_DIR=" + str(prefix)]
        if layout == "linux":
            arguments += ["-Ccmake.define.CMAKE_CXX_FLAGS=-static-libstdc++ -static-libgcc"]
        run([*arguments, ROOT / "python"])
        wheel, = raw.glob("*.whl")
        tools = python.parent
        if layout == "linux":
            run([tools / "auditwheel", "repair", "--plat", "manylinux_2_28_" + os.environ["PACKAGE_ARCH"], "-w", DIST, wheel])
        elif layout == "macos":
            run([tools / "delocate-wheel", "--require-archs", "arm64", "-w", DIST, wheel])
        else:
            run([python, "-m", "delvewheel", "repair", "--ignore-in-wheel", "--add-path", prefix / "lib", "-w", DIST, wheel])
        repaired = next(path for path in DIST.glob("*.whl") if ("cp" + abi) in path.name)
        checked = WORK / ("checked-wheel-" + abi)
        with zipfile.ZipFile(repaired) as stream:
            stream.extractall(checked)
        _, symbols = packaging_tools()
        symbols.verify_stripped(checked)
        if abi == "312":
            run([python, "-m", "abi3audit", "--strict", repaired])


def node_package(prefix, metadata):
    package = ROOT / "typescript"
    run(["npm", "ci", "--ignore-scripts"], cwd=package)
    run(["node", "scripts/build-addon.mjs"], cwd=package)
    run(["node", "node_modules/typescript/bin/tsc", "-p", "tsconfig.json"], cwd=package)
    native = package / "dist/native"
    shutil.copytree(prefix / "lib", native / "bin")
    shutil.copy2(build_output(package / "build", "seekdb_native.node"), native / "bin/seekdb_native.node")
    for name in ("etc", "share", "libexec", "licenses"):
        if (prefix / name).is_dir():
            shutil.copytree(prefix / name, native / name)
    shutil.copy2(prefix / "BUILD-INFO.json", native / "BUILD-INFO.json")
    repair_native(native / "bin")
    platform = {"platform": {"macos": "darwin", "windows": "win32"}.get(os.environ["PACKAGE_OS"], "linux"),
                "arch": "x64" if os.environ["PACKAGE_ARCH"] == "x86_64" else "arm64"}
    if os.environ["PACKAGE_OS"] == "linux":
        platform.update(libc="glibc", minimumGlibc="2.28")
    write_json(native / "platform.json", platform)
    run(["node", "scripts/check-package.mjs"], cwd=package)
    run(["node", "--test", "test/binding.test.mjs"], cwd=package)
    manifest = json.loads((package / "package.json").read_text())
    manifest.update(version=metadata["package_versions"]["node"], os=[platform["platform"]], cpu=[platform["arch"]])
    for name in ("private", "scripts", "devDependencies"):
        manifest.pop(name, None)
    if platform.get("libc"):
        manifest["libc"] = ["glibc"]
    staging = WORK / "npm"
    staging.mkdir()
    write_json(staging / "package.json", manifest)
    for name in ("dist",):
        shutil.copytree(package / name, staging / name)
    shutil.copy2(package / "README.md", staging / "README.md")
    shutil.copy2(ROOT / "LICENSE", staging / "LICENSE")
    result = subprocess.check_output(npm_command("pack", "--ignore-scripts", "--json", "--pack-destination", str(DIST)), cwd=staging, text=True)
    packed = DIST / json.loads(result)[0]["filename"]
    packed.rename(DIST / (packed.stem + "-" + metadata["target"] + "-" + metadata["built_at"] +
                         "-sdb" + metadata["seekdb_commit"][:12] + "-bnd" + metadata["bindings_commit"][:12] + ".tgz"))


def java_package(prefix, metadata):
    classes = WORK / "java/classes"
    classes.mkdir(parents=True)
    run(["javac", "--release", "8", "-g:none", "-d", classes,
         *sorted((ROOT / "java/src/main/java/com/oceanbase/seekdb").glob("*.java"))])
    jar = WORK / "java/seekdb-java.jar"
    component = "android" if os.environ["PACKAGE_OS"] == "android" else "java"
    manifest = WORK / "java/MANIFEST.MF"
    manifest.write_text("Manifest-Version: 1.0\nImplementation-Title: seekdb\nImplementation-Version: " +
                        metadata["package_versions"][component] + "\nseekdb-engine-version: " +
                        metadata["engine_version"] + "\n\n", encoding="utf-8")
    run(["jar", "cfm", jar, manifest, "-C", classes, "."])
    if os.environ["PACKAGE_OS"] == "android":
        stage = WORK / "aar"
        stage.mkdir()
        shutil.copy2(jar, stage / "classes.jar")
        for name in ("AndroidManifest.xml", "proguard.txt"):
            shutil.copy2(ROOT / "android/aar" / name, stage / name)
        native = stage / "jni/arm64-v8a"
        native.mkdir(parents=True)
        for source in (prefix / "lib").glob("*.so"):
            shutil.copy2(source, native / source.name)
        shutil.copy2(prefix / "lib/seekdb", native / "libseekdb_exec.so")
        assets = stage / "assets/seekdb"
        assets.mkdir(parents=True)
        for resource in ("etc", "share", "libexec", "licenses"):
            if (prefix / resource).is_dir():
                shutil.copytree(prefix / resource, assets / resource)
        shutil.copy2(prefix / "BUILD-INFO.json", assets / "BUILD-INFO.json")
        name = "seekdb-android-%s-arm64-api28-%s-sdb%s-bnd%s.aar" % (metadata["package_versions"]["android"], metadata["built_at"],
            metadata["seekdb_commit"][:12], metadata["bindings_commit"][:12])
        with zipfile.ZipFile(DIST / name, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as stream:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    stream.write(path, path.relative_to(stage).as_posix())
    else:
        stage = WORK / "java/package"
        shutil.copytree(prefix, stage)
        shutil.copy2(jar, stage / "seekdb-java.jar")
        archive(stage, "seekdb-java", metadata, "java")


def build():
    if os.environ["PACKAGE_OS"] == "macos":
        os.environ["TMPDIR"] = "/tmp"
        os.environ.setdefault("SDKROOT", subprocess.check_output(
            ["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip())
    elif os.environ["PACKAGE_OS"] == "linux":
        # Keep the manylinux image's compiler/toolchain PATH.
        os.environ["PATH"] = "/opt/node/bin:/opt/java/bin:/opt/python/cp312-cp312/bin:" + os.environ["PATH"]
    WORK.mkdir(parents=True, exist_ok=True)
    DIST.mkdir(exist_ok=True)
    engine, manifest, metadata = runtime()
    metadata["package_versions"] = package_versions(manifest["version"])
    prefix = sdk(engine, metadata)
    if os.environ["PACKAGE_OS"] != "android":
        if selected("python"):
            python_wheels(prefix, metadata)
        if selected("node"):
            node_package(prefix, metadata)
    if selected("java"):
        java_package(prefix, metadata)
    write_json(DIST / (metadata["target"] + "-provenance.json"), metadata)
    if os.environ["PACKAGE_OS"] == "android":
        report = {"target": metadata["target"], "packages": [], "status": "not_run",
            "source_commit": metadata["bindings_commit"],
            "reason": "No arm64 Android runtime available; cross-compilation and ELF checks only"}
        write_json(DIST / (metadata["target"] + "-smoke.json"), report)
        print(json.dumps(report), flush=True)
    for path in sorted(DIST.iterdir()):
        if path.is_file() and not path.name.endswith(".sha256"):
            path.with_name(path.name + ".sha256").write_text(digest(path) + "  " + path.name + "\n")


def container(command="build"):
    arch = os.environ["PACKAGE_ARCH"]
    image = "seekdb-bindings-manylinux-" + arch
    if command == "build":
        run(["docker", "build", "--tag", image, "--build-arg",
             "BASE_IMAGE=quay.io/pypa/manylinux_2_28_" + arch + ":2026.03.20-1", "-"],
            input=(ROOT / "scripts/docker/bindings-manylinux.Dockerfile").read_text(), text=True)
    node = Path(shutil.which("node")).resolve().parent.parent
    arguments = ["docker", "run", "--rm", "--user", "%s:%s" % (os.getuid(), os.getgid()),
        "-e", "HOME=/tmp", "-v", str(ROOT) + ":/project", "-w", "/project",
        "-v", str(node) + ":/opt/node:ro"]
    if selected("java"):
        arguments += ["-v", os.environ["JAVA_HOME"] + ":/opt/java:ro", "-e", "JAVA_HOME=/opt/java"]
    for key in ("PACKAGE_OS", "PACKAGE_ARCH", "PACKAGE_TARGET", "PACKAGE_COMPONENT", "PACKAGE_TIMESTAMP",
                "BINARY_SOURCE_COMMIT", "BINARY_REPOSITORY", "BINARY_RUN_ID", "BUILD_JOBS", *VERSION_ENV):
        arguments += ["-e", key + "=" + os.environ.get(key, "")]
    arguments += ["-e", "PIP_CACHE_DIR=/project/deps/.cache/pip", "-e", "npm_config_cache=/project/deps/.cache/npm",
        "-e", "PYTHON_311=/opt/python/cp311-cp311/bin/python", "-e", "PYTHON_312=/opt/python/cp312-cp312/bin/python",
        image,
        "/opt/python/cp312-cp312/bin/python", ".github/scripts/package.py", command]
    run(arguments)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = {"matrix": matrix, "build": build, "container": container,
                "smoke": lambda: run([sys.executable, ROOT / ".github/scripts/smoke_packages.py"]),
                "container-smoke": lambda: container("smoke")}
    parser.add_argument("command", choices=commands)
    arguments = parser.parse_args()
    commands[arguments.command]()


if __name__ == "__main__":
    main()
