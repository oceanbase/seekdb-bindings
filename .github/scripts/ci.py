#!/usr/bin/env python3
"""Run binding CI against an existing, verified SeekDB binary distribution."""

import argparse
from datetime import datetime, timezone
import importlib.machinery
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import package as release

ROOT = release.ROOT


def select_engine():
    repository = os.environ["BINARY_REPOSITORY"]
    workflow = "repos/%s/actions/workflows/package.yml/runs" % repository
    runs = release.api(workflow + "?branch=master&event=workflow_dispatch&status=success&per_page=100")
    artifact = "binary-linux-x86_64-glibc2.28"
    for source in runs["workflow_runs"]:
        artifacts = release.api("repos/%s/actions/runs/%s/artifacts?per_page=100" % (repository, source["id"]))
        if any(item["name"] == artifact and not item["expired"] for item in artifacts["artifacts"]):
            release.output("run_id", str(source["id"]))
            release.output("artifact", artifact)
            release.output("source_commit", source["head_sha"])
            release.output("packaging_ref", release.PACKAGING_REF)
            release.output("dependency_key", release.dependency_key())
            release.output("timestamp", datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
            print("Reusing " + source["html_url"], flush=True)
            return
    raise ValueError("No unexpired %s from a successful master packaging run; run Package seekdb for Linux x86_64/glibc 2.28 first" % artifact)


def sdk_directory():
    return Path(os.environ["RUNNER_TEMP"]) / "seekdb-test-sdk"


def prepare_runtime():
    release.WORK = Path(os.environ["RUNNER_TEMP"]) / "seekdb-input"
    release.WORK.mkdir()
    engine, _, metadata = release.runtime()
    prefix = sdk_directory()
    release.stage_engine(engine, prefix)
    release.repair_native(prefix / "lib")
    metadata["purpose"] = "binding-ci"
    release.write_json(prefix / "BUILD-INFO.json", metadata)
    # Keep the relative library/resource layout beside the freshly built driver.
    shutil.copytree(prefix, (ROOT / os.environ["BUILD_DIR"]).parent)
    release.run([prefix / "lib/seekdb", "-V"])
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as stream:
        stream.write("SEEKDB_BIN=" + str(prefix / "lib/seekdb") + "\n")


def python_tests():
    # Package the debug driver under test together with the complete engine SDK.
    prefix = sdk_directory()
    shutil.copy2(ROOT / os.environ["BUILD_DIR"] / "libseekdb.so", prefix / "lib/libseekdb.so")
    release.run([sys.executable, "-m", "pip", "install", "--no-build-isolation",
                 "-Ccmake.define.SEEKDB_SDK_DIR=" + str(prefix), ROOT / "python"])
    for test in sorted((ROOT / "python/tests").glob("*_test.py")):
        release.run([sys.executable, test])


def python_contract():
    build = ROOT / os.environ["BUILD_DIR"]
    candidates = {path for suffix in importlib.machinery.EXTENSION_SUFFIXES
                  for path in build.glob("pylibseekdb*" + suffix)}
    if len(candidates) != 1:
        raise ValueError("Expected one built Python extension: " + repr(candidates))
    extension, = candidates
    with tempfile.TemporaryDirectory() as temporary:
        package = Path(temporary) / "pylibseekdb"
        package.mkdir()
        shutil.copy2(ROOT / "python/pylibseekdb/__init__.py", package / "__init__.py")
        shutil.copy2(extension, package / extension.name)
        shutil.copy2(build / "libseekdb.so", package / "libseekdb.so")
        sys.path.insert(0, temporary)
        import pylibseekdb
        assert issubclass(pylibseekdb.SeekdbError, RuntimeError)
        for name in ("SeekdbError", "Connection", "Cursor"):
            cls = getattr(pylibseekdb, name)
            assert cls.__module__ == "pylibseekdb"
            assert str(cls) == "<class 'pylibseekdb.%s'>" % name


def pure_c():
    build = ROOT / os.environ["BUILD_DIR"]
    library = build / "libseekdb.so"
    if not library.is_file() or not (build / "libseekdb_driver.so").is_symlink():
        raise ValueError("Missing C driver or its compatibility symlink")
    dynamic = subprocess.check_output(["readelf", "-d", str(library)], text=True)
    if re.search(r"NEEDED.*lib(?:std)?c\+\+", dynamic):
        raise ValueError("libseekdb must not depend on a C++ runtime: " + dynamic)
    print("C driver has no C++ runtime dependency")


def main():
    commands = {"select-engine": select_engine, "runtime": prepare_runtime,
                "python-tests": python_tests, "python-contract": python_contract, "pure-c": pure_c}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=commands)
    commands[parser.parse_args().command]()


if __name__ == "__main__":
    main()
