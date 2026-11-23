#!/usr/bin/env python3
"""Install or unpack the finished distributions and smoke-test them before upload."""

from contextlib import contextmanager
import argparse
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tarfile
import tempfile
import time
import zipfile

import package

ROOT = package.ROOT
SOURCES = Path(__file__).with_name("smoke")


def run(arguments, cwd, env, **kwargs):
    return package.run(arguments, cwd=cwd, env=env, timeout=180, **kwargs)


def unpack(archive, destination):
    destination.mkdir(parents=True)
    with tarfile.open(archive) as stream:
        stream.extractall(destination, filter="data")
    return destination


def one(pattern):
    result, = package.DIST.glob(pattern)
    return result


def environment(native=None):
    result = {key: value for key, value in os.environ.items()
              if not key.startswith(("DYLD_", "LD_")) and key not in ("PYTHONPATH", "SEEKDB_BIN")}
    if native and os.name == "nt":
        result["PATH"] = str(native) + os.pathsep + result["PATH"]
    return result


def remove(directory):
    # Closing the last client releases the server asynchronously. Windows keeps
    # the copied executable locked until that process has actually exited.
    deadline = time.monotonic() + 30
    def writable(function, path, error):
        if os.name != "nt" or not isinstance(error, PermissionError):
            raise error
        os.chmod(path, stat.S_IWRITE)
        function(path)
    while directory.exists():
        try:
            shutil.rmtree(directory, onexc=writable)
        except PermissionError:
            if os.name != "nt" or time.monotonic() >= deadline:
                raise
            time.sleep(0.5)


@contextmanager
def consumer_directory():
    root = Path(tempfile.mkdtemp(prefix="seekdb-package-smoke-"))
    try:
        yield root
    finally:
        remove(root)


@contextmanager
def hidden_build_tree():
    source = ROOT / "build"
    with tempfile.TemporaryDirectory(prefix="seekdb-hidden-build-", dir=ROOT) as temporary:
        destination = Path(temporary) / "build"
        if source.exists():
            source.rename(destination)
        try:
            yield
        finally:
            if destination.exists():
                destination.rename(source)


def native_smoke(prefix, work):
    native = prefix / "lib"
    env = environment(native)
    run([native / package.executable_name("seekdb"), "-V"], work, env)
    client = work / package.executable_name("client")
    source = SOURCES / "native.c"
    if os.name == "nt":
        command = ["cl", "/nologo", "/MD", "/I" + str(prefix / "include"), source,
                   native / "seekdb.lib", "/Fe:" + str(client)]
    else:
        command = ["cc", "-I" + str(prefix / "include"), source, "-L" + str(native), "-lseekdb",
                   "-Wl,-rpath," + str(native), "-o", client]
    run(command, work, env)
    run([client, work / "database"], work, env)
    cli = native / package.executable_name("seekdb_cli")
    result = run([cli, work / "cli-database"], work, env,
                 input="SELECT 314159 AS smoke_value;\nquit\n", text=True, capture_output=True)
    if "314159" not in result.stdout or "ERROR:" in result.stderr:
        raise ValueError("Packaged CLI smoke failed: " + result.stdout + result.stderr)
    remove(work / "database")
    remove(work / "cli-database")


def python_smoke(root, metadata):
    for abi in ("311", "312"):
        work = root / ("python-" + abi)
        work.mkdir()
        venv = work / "venv"
        env = environment()
        run([os.environ["PYTHON_" + abi], "-m", "venv", venv], work, env)
        python = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        run([python, "-m", "pip", "install", "--only-binary=:all:", one("*cp" + abi + "*.whl")], work, env)
        run([python, "-I", SOURCES / "python.py", metadata["package_versions"]["python"]], work, env)
        run([python, "-I", ROOT / "python/tests/seekdb_test.py"], work, env)
        remove(work)


def node_smoke(root, metadata):
    work = root / "npm"
    work.mkdir()
    env = environment()
    package.write_json(work / "package.json", {"private": True, "type": "module"})
    run(["npm", "install", "--ignore-scripts", "--no-audit", "--no-fund", one("*.tgz"), "mysql2@^3"], work, env)
    source = work / "smoke.mjs"
    shutil.copy2(SOURCES / "node.mjs", source)
    run(["node", source, metadata["package_versions"]["node"]], work, env)
    remove(work)


def java_smoke(root, metadata):
    work = root / "java"
    work.mkdir()
    prefix = unpack(one("seekdb-java-*.tar.gz"), work / "package")
    with zipfile.ZipFile(prefix / "seekdb-java.jar") as stream:
        manifest = stream.read("META-INF/MANIFEST.MF").decode()
    if "Implementation-Version: " + metadata["package_versions"]["java"] not in manifest:
        raise ValueError("Packaged Java version mismatch")
    run(["java", "-Djava.library.path=" + str(prefix / "lib"), "--class-path", prefix / "seekdb-java.jar",
         ROOT / "java/test/com/oceanbase/seekdb/test/LifecycleSmoke.java", work / "database"],
        work, environment(prefix / "lib"))
    # Exercise SQL through this archive's driver and headers as well as JNI lifecycle.
    native_smoke(prefix, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=package.DIST)
    package.DIST = parser.parse_args().dist.resolve()
    layout = os.environ["PACKAGE_OS"]
    if layout == "linux":
        os.environ["PATH"] = "/opt/node/bin:/opt/java/bin:/opt/python/cp312-cp312/bin:" + os.environ["PATH"]
    if layout == "android":
        raise ValueError("Android runtime smoke requires an Android runner")
    metadata = json.loads(one("*-provenance.json").read_text())
    for path in package.DIST.iterdir():
        if path.is_file() and not path.name.endswith(".sha256"):
            expected = path.with_name(path.name + ".sha256").read_text().split()
            if expected != [package.digest(path), path.name]:
                raise ValueError("Finished distribution checksum mismatch: " + path.name)
    report = {"target": metadata["target"], "packages": [], "source_commit": metadata["bindings_commit"]}
    with consumer_directory() as root, hidden_build_tree():
        sdk = root / "sdk"
        sdk.mkdir()
        prefix = unpack(one("libseekdb-*.tar.gz"), sdk / "package")
        native_smoke(prefix, sdk)
        report["packages"].append("sdk")
        if package.selected("python"):
            python_smoke(root, metadata)
            report["packages"].extend(("python-cp311", "python-cp312-abi3"))
        if package.selected("node"):
            node_smoke(root, metadata)
            report["packages"].append("node")
        if package.selected("java"):
            java_smoke(root, metadata)
            report["packages"].append("java")
    report["status"] = "passed"
    output = package.DIST / (metadata["target"] + "-smoke.json")
    package.write_json(output, report)
    output.with_name(output.name + ".sha256").write_text(package.digest(output) + "  " + output.name + "\n")
    print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
