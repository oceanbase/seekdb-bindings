#!/usr/bin/env python3
"""Prepare tested bindings distributions for PyPI/npm, then publish without rebuilding."""

import argparse
import base64
from email.parser import BytesParser
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile
from urllib.error import HTTPError
from urllib.parse import quote
from urllib.request import urlopen
import zipfile

from package import ROOT, digest, output, write_json
from release import collect, safe_name

PYPI_NAME = "pylibseekdb"
NPM_NAME = "@oceanbase/seekdb-node"
NPM_REGISTRY = "https://registry.npmjs.org"
NPM_PLATFORMS = {
    "linux-x86_64": ("linux-x64-gnu", "linux", "x64"),
    "linux-aarch64": ("linux-arm64-gnu", "linux", "arm64"),
    "macos-arm64": ("darwin-arm64", "darwin", "arm64"),
    "windows-x86_64": ("win32-x64", "win32", "x64"),
}


def registry_json(url):
    try:
        with urlopen(url, timeout=60) as response:
            return json.load(response)
    except HTTPError as error:
        if error.code == 404:
            return None
        raise


def wheel_metadata(path, expected_version):
    from packaging.utils import canonicalize_name, parse_wheel_filename
    from packaging.version import Version

    name, version, _, _ = parse_wheel_filename(path.name)
    with zipfile.ZipFile(path) as stream:
        metadata_files = [name for name in stream.namelist() if name.endswith(".dist-info/METADATA")]
        if len(metadata_files) != 1:
            raise ValueError("Wheel must contain exactly one METADATA: " + path.name)
        metadata = BytesParser().parsebytes(stream.read(metadata_files[0]))
    if (canonicalize_name(name) != PYPI_NAME or canonicalize_name(metadata["Name"]) != PYPI_NAME or
            version != Version(expected_version) or Version(metadata["Version"]) != version):
        raise ValueError("Wheel name/version differs from the tested package: " + path.name)


def npm_files(path):
    files = {}
    with tarfile.open(path, "r:gz") as stream:
        for member in stream:
            name = PurePosixPath(member.name)
            if (name.is_absolute() or ".." in name.parts or not name.parts or name.parts[0] != "package" or
                    "\\" in member.name or not (member.isfile() or member.isdir())):
                raise ValueError("Unsafe npm archive member: " + member.name)
            if member.isdir():
                continue
            relative = name.relative_to("package").as_posix()
            if relative in files:
                raise ValueError("Duplicate npm archive member: " + member.name)
            files[relative] = (stream.extractfile(member).read(), member.mode & 0o777)
    return files


def npm_archive(directory, manifest, files):
    files = dict(files)
    files["package.json"] = ((json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode(), 0o644)
    filename = manifest["name"].removeprefix("@").replace("/", "-") + "-" + manifest["version"] + ".tgz"
    destination = directory / filename
    # Stable tar/gzip metadata makes a retry byte-identical to the first attempt.
    with destination.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as stream:
            for name, (data, mode) in sorted(files.items()):
                member = tarfile.TarInfo("package/" + name)
                member.size = len(data)
                member.mode = mode
                member.uid = member.gid = 0
                member.uname = member.gname = "root"
                stream.addfile(member, io.BytesIO(data))
    return destination


def npm_packages(source, assets, directory, version):
    platforms = [item for item in source["platforms"] if "node" in item["components"]]
    if {item["platform"] for item in platforms} != set(NPM_PLATFORMS):
        raise ValueError("npm publication requires Linux x86_64/ARM64, macOS ARM64 and Windows x86_64 node packages")
    packages = []
    dependencies = {}
    shared = None
    base_manifest = None
    for platform in platforms:
        filename, = [name for name in platform["packages"] if name.endswith(".tgz")]
        files = npm_files(assets / filename)
        manifest = json.loads(files["package.json"][0])
        suffix, system, arch = NPM_PLATFORMS[platform["platform"]]
        if (manifest["name"] != NPM_NAME or manifest["version"] != version or manifest.get("private") or
                manifest.get("scripts") or manifest.get("os") != [system] or manifest.get("cpu") != [arch]):
            raise ValueError("npm manifest differs from the tested platform/version: " + filename)
        native = json.loads(files["dist/native/platform.json"][0])
        if native["platform"] != system or native["arch"] != arch:
            raise ValueError("npm native platform metadata differs: " + filename)
        if system == "linux" and (manifest.get("libc") != ["glibc"] or native.get("libc") != "glibc"):
            raise ValueError("Linux npm package must declare glibc")
        # Windows checkouts/compiler output can use CRLF and different file modes.
        # Normalize only the shared text; platform archives retain every payload byte.
        public = {name: (files[name][0].replace(b"\r\n", b"\n"), 0o644)
                  for name in ("dist/index.js", "dist/index.d.ts", "README.md", "LICENSE")}
        if shared is not None and public != shared:
            different = [name for name in public if public[name] != shared[name]]
            raise ValueError("npm platforms have different public files: " + ", ".join(different))
        shared = public
        base_manifest = dict(manifest)
        manifest["name"] = NPM_NAME + "-" + suffix
        manifest["repository"] = {"type": "git", "url": "git+https://github.com/oceanbase/seekdb-bindings.git"}
        manifest["publishConfig"] = {"access": "public", "registry": NPM_REGISTRY}
        dependencies[manifest["name"]] = version
        packed = npm_archive(directory, manifest, files)
        packages.append({"name": manifest["name"], "version": version, "file": packed.name, "platform": platform["platform"]})
    for key in ("os", "cpu", "libc"):
        base_manifest.pop(key, None)
    base_manifest["repository"] = {"type": "git", "url": "git+https://github.com/oceanbase/seekdb-bindings.git"}
    base_manifest["publishConfig"] = {"access": "public", "registry": NPM_REGISTRY}
    base_manifest["optionalDependencies"] = dict(sorted(dependencies.items()))
    base_manifest["files"] = ["dist/index.js", "dist/index.d.ts", "README.md", "LICENSE"]
    shared["dist/index.js"] = (Path(__file__).with_name("npm-entry.mjs").read_bytes(), 0o644)
    packed = npm_archive(directory, base_manifest, shared)
    # Publish platform packages first, so the public package's dependencies exist.
    packages.append({"name": NPM_NAME, "version": version, "file": packed.name})
    return packages


def prepare(registry, repository, run_ids, directory):
    directory.mkdir(parents=True)
    with tempfile.TemporaryDirectory() as temporary:
        assets = Path(temporary) / "assets"
        source = collect(repository, run_ids, assets)
        component = "python" if registry == "pypi" else "node"
        version = source["package_versions"].get(component)
        if not version:
            raise ValueError("Selected packaging runs have no tested " + component + " packages")
        if registry == "pypi":
            packages = []
            for platform in source["platforms"]:
                for name in platform["packages"]:
                    if name.endswith(".whl"):
                        wheel_metadata(assets / name, version)
                        shutil.copyfile(assets / name, directory / name)
                        packages.append({"name": PYPI_NAME, "version": version, "file": name, "platform": platform["platform"]})
        else:
            packages = npm_packages(source, assets, directory, version)
    for package in packages:
        path = directory / package["file"]
        package["sha256"] = digest(path)
        if registry == "npm":
            package["integrity"] = "sha512-" + base64.b64encode(hashlib.sha512(path.read_bytes()).digest()).decode()
    manifest = {"schema_version": 1, "registry": registry, "version": version, "packages": packages,
                "bindings_commit": source["bindings_commit"], "engine_version": source["engine_version"],
                "platforms": source["platforms"], "repository": repository}
    write_json(directory / "publication.json", manifest)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as stream:
            stream.write(f"## {registry}: {version}\n\n| Package | File |\n| --- | --- |\n")
            for package in packages:
                stream.write(f"| `{package['name']}` | `{package['file']}` |\n")
    print(f"Prepared {len(packages)} {registry} packages at version {version}", flush=True)
    return manifest


def load_prepared(directory, registry):
    manifest = json.loads((directory / "publication.json").read_text())
    if manifest["registry"] != registry or manifest["repository"] != os.environ["GITHUB_REPOSITORY"]:
        raise ValueError("Prepared publication belongs to a different registry or repository")
    expected = {"publication.json"}
    for package in manifest["packages"]:
        name = package["file"]
        if package["version"] != manifest["version"]:
            raise ValueError("Prepared package versions differ")
        safe_name(name)
        if name in expected:
            raise ValueError("Duplicate publication file: " + name)
        expected.add(name)
        path = directory / name
        if path.is_symlink() or not path.is_file() or digest(path) != package["sha256"]:
            raise ValueError("Publication file checksum mismatch: " + name)
        if registry == "pypi":
            wheel_metadata(path, manifest["version"])
        else:
            metadata = json.loads(npm_files(path)["package.json"][0])
            allowed = {NPM_NAME, *[NPM_NAME + "-" + value[0] for value in NPM_PLATFORMS.values()]}
            if package["name"] not in allowed or (metadata["name"], metadata["version"]) != (package["name"], manifest["version"]):
                raise ValueError("Unexpected npm package name or version")
            integrity = "sha512-" + base64.b64encode(hashlib.sha512(path.read_bytes()).digest()).decode()
            if package["integrity"] != integrity:
                raise ValueError("npm integrity mismatch: " + name)
    if not manifest["packages"] or expected != {path.name for path in directory.iterdir()}:
        raise ValueError("Prepared publication file inventory mismatch")
    return manifest


def pending(directory, registry):
    manifest = load_prepared(directory, registry)
    uploads = []
    if registry == "pypi":
        remote = registry_json(f"https://pypi.org/pypi/{PYPI_NAME}/{quote(manifest['version'], safe='')}/json")
        existing = {item["filename"]: item["digests"]["sha256"] for item in remote["urls"]} if remote else {}
        for package in manifest["packages"]:
            if package["file"] not in existing:
                uploads.append(package)
            elif existing[package["file"]] != package["sha256"]:
                raise ValueError("PyPI already has different bytes for " + package["file"])
    else:
        for package in manifest["packages"]:
            remote = registry_json(NPM_REGISTRY + "/" + quote(package["name"], safe="") + "/" + quote(package["version"], safe=""))
            if remote is None:
                uploads.append(package)
            elif remote["dist"].get("integrity") != package["integrity"]:
                raise ValueError("npm already has different bytes for " + package["name"] + "@" + package["version"])
    print(f"{len(uploads)} packages pending; {len(manifest['packages']) - len(uploads)} identical packages already published", flush=True)
    return manifest, uploads


def pypi_uploads(directory):
    _, uploads = pending(directory, "pypi")
    target = directory.parent / "pypi-upload"
    target.mkdir()
    for package in uploads:
        shutil.copyfile(directory / package["file"], target / package["file"])
    output("count", str(len(uploads)))


def publish_npm(directory):
    manifest, uploads = pending(directory, "npm")
    tag = npm_tag(manifest)
    for package in uploads:
        subprocess.run(["npm", "publish", str((directory / package["file"]).resolve()), "--access=public", "--tag=" + tag,
                        "--ignore-scripts", "--provenance", "--registry=" + NPM_REGISTRY], check=True)


def npm_tag(manifest):
    tag = os.environ["NPM_DIST_TAG"]
    if tag not in ("latest", "next") or (tag == "latest" and "-" in manifest["version"]):
        raise ValueError("Use next for npm prereleases, or latest for stable releases")
    return tag


def check_npm(directory):
    manifest, _ = pending(directory, "npm")
    tag = npm_tag(manifest)
    for package in manifest["packages"]:
        subprocess.run(["npm", "publish", str((directory / package["file"]).resolve()), "--access=public", "--tag=" + tag,
                        "--ignore-scripts", "--dry-run", "--json", "--registry=" + NPM_REGISTRY],
                       check=True, stdout=subprocess.DEVNULL)
        print("Validated npm tarball: " + package["file"], flush=True)


def smoke_npm(directory):
    from smoke_packages import SOURCES, consumer_directory, environment, run

    manifest = load_prepared(directory, "npm")
    system, arch = json.loads(subprocess.check_output(
        ["node", "-p", "JSON.stringify([process.platform, process.arch])"], text=True))
    suffix, = [value[0] for value in NPM_PLATFORMS.values() if value[1:] == (system, arch)]
    native, = [item for item in manifest["packages"] if item["name"] == NPM_NAME + "-" + suffix]
    public, = [item for item in manifest["packages"] if item["name"] == NPM_NAME]
    with consumer_directory() as work:
        write_json(work / "package.json", {"private": True, "type": "module"})
        # Install both local archives explicitly before their first registry upload.
        # The root dependency supplies the platform package without registry lookup.
        run(["npm", "install", "--ignore-scripts", "--omit=optional", "--no-audit", "--no-fund",
             (directory / native["file"]).resolve(), (directory / public["file"]).resolve(), "mysql2@^3"], work, environment())
        shutil.copyfile(SOURCES / "node.mjs", work / "smoke.mjs")
        run(["node", work / "smoke.mjs", manifest["version"]], work, environment())


def main():


    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare-pypi", "prepare-npm", "check-pypi", "check-npm", "pypi-uploads", "publish-npm", "smoke-npm"))
    parser.add_argument("--directory", type=Path)
    arguments = parser.parse_args()
    registry = "pypi" if "pypi" in arguments.command else "npm"
    directory = arguments.directory or ROOT / "build/registry" / registry
    if arguments.command.startswith("prepare-"):
        run_ids = [int(value) for value in re.split(r"[\s,]+", os.environ["PACKAGE_RUN_IDS"].strip()) if value]
        prepare(registry, os.environ["GITHUB_REPOSITORY"], run_ids, directory)
    elif arguments.command == "pypi-uploads":
        pypi_uploads(directory)
    elif arguments.command == "publish-npm":
        publish_npm(directory)
    elif arguments.command == "check-npm":
        check_npm(directory)
    elif arguments.command == "smoke-npm":
        smoke_npm(directory)
    else:
        pending(directory, registry)


if __name__ == "__main__":
    main()
