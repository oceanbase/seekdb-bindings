#!/usr/bin/env python3
"""Publish verified bindings artifacts to GitHub Releases without rebuilding them."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
from urllib.parse import quote
import zipfile

from package import ROOT, TARGETS, NPM_PLATFORMS, digest, write_json


def gh(*arguments, **kwargs):
    return subprocess.run(["gh", *map(str, arguments)], check=True, **kwargs)


def api(path, missing_ok=False, method="GET", data=None):
    arguments = ["gh", "api", "--method", method, path]
    if data is not None:
        arguments += ["--input", "-"]
    result = subprocess.run(arguments, input=json.dumps(data) if data is not None else None,
                            text=True, capture_output=True)
    if result.returncode:
        if missing_ok and "(HTTP 404)" in result.stderr:
            return None
        raise RuntimeError("GitHub API request failed: " + result.stderr.strip())
    return json.loads(result.stdout) if result.stdout.strip() else None


def validate_tag(tag):
    if not tag or tag.startswith("-"):
        raise ValueError("Provide a release tag such as v1.4.0")
    subprocess.run(["git", "check-ref-format", "refs/tags/" + tag], check=True, capture_output=True)


def check_tag(repository, tag, commit):
    reference = api(f"repos/{repository}/git/ref/tags/{quote(tag, safe='')}", missing_ok=True)
    if reference is None:
        return False
    target = reference["object"]
    while target["type"] == "tag":
        target = api(f"repos/{repository}/git/tags/{target['sha']}")["object"]
    if target["type"] != "commit" or target["sha"] != commit:
        raise ValueError("Release tag points to a different bindings commit")
    return True


def ensure_tag(repository, tag, commit):
    if check_tag(repository, tag, commit):
        return
    try:
        api(f"repos/{repository}/git/refs", method="POST", data={"ref": "refs/tags/" + tag, "sha": commit})
    except RuntimeError as error:
        # A concurrent publisher may have created it after our read. Never update a ref.
        if check_tag(repository, tag, commit):
            return
        if "(HTTP 403)" in str(error):
            raise RuntimeError(str(error) + "\nFor a workflow-changing target without an existing ref, "
                               "create the tag beforehand or configure RELEASE_TOKEN with Contents and Workflows write.") from error
        raise
    if not check_tag(repository, tag, commit):
        raise ValueError("Created release tag is not available; retry the release")


def safe_name(name):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", name):
        raise ValueError("Unexpected artifact filename: " + name)
    return name


def download_artifact(repository, artifact_id, destination):
    destination.mkdir()
    with tempfile.TemporaryFile() as downloaded:
        gh("api", f"repos/{repository}/actions/artifacts/{artifact_id}/zip", stdout=downloaded)
        downloaded.seek(0)
        with zipfile.ZipFile(downloaded) as archive:
            for member in archive.infolist():
                name = safe_name(member.filename)
                if member.is_dir() or stat.S_ISLNK(member.external_attr >> 16):
                    raise ValueError("Artifact must contain only regular files")
                with archive.open(member) as source, (destination / name).open("xb") as output:
                    shutil.copyfileobj(source, output)


def verify_bundle(directory, platform, commit):
    files = {path.name for path in directory.iterdir()}
    for name in files:
        safe_name(name)
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("Artifact must contain only regular files")
        if name.endswith(".sha256"):
            if name.removesuffix(".sha256") not in files:
                raise ValueError("Checksum has no corresponding file: " + name)
        else:
            checksum = directory / (name + ".sha256")
            if not checksum.is_file() or checksum.read_text().split() != [digest(path), name]:
                raise ValueError("Artifact checksum mismatch: " + name)
    provenances = list(directory.glob("*-provenance.json"))
    if len(provenances) != 1:
        raise ValueError("Artifact must contain exactly one provenance file")
    provenance, = provenances
    metadata = json.loads(provenance.read_text())
    if any(not re.fullmatch(r"[0-9][0-9A-Za-z.+_-]*", value) for value in metadata["package_versions"].values()):
        raise ValueError("Invalid package versions in provenance")
    target = metadata["target"]
    if not target.startswith(platform + "-") or provenance.name != target + "-provenance.json":
        raise ValueError("Artifact platform and provenance do not match")
    if metadata["bindings_commit"] != commit:
        raise ValueError("Artifact bindings commit does not match its workflow run")
    report_name = target + "-smoke.json"
    report = json.loads((directory / report_name).read_text())
    if report["status"] != "passed" or report["target"] != target or report["source_commit"] != commit:
        raise ValueError("Artifact has no passed smoke report for this source commit")
    components = set(report["packages"])
    patterns = {
        "sdk": f"libseekdb-{metadata['package_versions']['sdk']}-{target}-*.tar.gz",
        "python-cp311": f"pylibseekdb-{metadata['package_versions']['python']}-cp311-cp311-*.whl",
        "python-cp312-abi3": f"pylibseekdb-{metadata['package_versions']['python']}-cp312-abi3-*.whl",
        "node": f"oceanbase-seekdb-node-{metadata['package_versions']['node']}-{target}-*.tgz",
        "java": f"seekdb-java-{metadata['package_versions']['java']}-{target}-*.tar.gz",
        "android-aar": f"seekdb-android-{metadata['package_versions']['android']}-arm64-api28-*.aar",
    }
    allowed = {"sdk", "android-aar"} if platform == "android-arm64" else set(patterns) - {"android-aar"}
    if "sdk" not in components or not components <= allowed:
        raise ValueError("Unexpected components in smoke report")
    if ("python-cp311" in components) != ("python-cp312-abi3" in components):
        raise ValueError("Both Python wheel ABIs must have passed smoke tests")
    payloads = []
    versions = {}
    for component in sorted(components):
        component_patterns = [patterns[component]]
        if component == "node" and "npm_packages" in metadata:
            suffix = NPM_PLATFORMS[platform][0]
            expected = {
                "platform": f"oceanbase-seekdb-node-{suffix}-{metadata['package_versions']['node']}-{target}-*.tgz",
                "main": f"oceanbase-seekdb-node-{metadata['package_versions']['node']}-*-bnd{commit[:12]}.tgz",
            }
            if set(metadata["npm_packages"]) != set(expected):
                raise ValueError("Expected npm platform and main packages")
            component_patterns = []
            for kind, pattern in expected.items():
                matches = list(directory.glob(pattern))
                if len(matches) != 1 or matches[0].name != metadata["npm_packages"][kind]:
                    raise ValueError("npm package inventory differs from provenance")
                component_patterns.append(pattern)
        for pattern in component_patterns:
            matches = list(directory.glob(pattern))
            if len(matches) != 1:
                raise ValueError(f"Expected one {component} package matching {pattern}")
            payloads.append(matches[0].name)
        key = {"python-cp311": "python", "python-cp312-abi3": "python", "android-aar": "android"}.get(component, component)
        versions[key] = metadata["package_versions"][key]
    expected = set(payloads) | {provenance.name, report_name}
    if files != expected | {name + ".sha256" for name in expected}:
        raise ValueError("Artifact files do not match the components that passed smoke tests")
    return metadata, versions, sorted(components), payloads


def release_notes(manifest):
    lines = [f"# seekdb bindings {manifest['tag']}", "",
             f"Bindings commit: `{manifest['bindings_commit']}`",
             f"Engine version: `{manifest['engine_version']}`", "", "Package versions:", ""]
    lines += [f"- {name}: `{version}`" for name, version in sorted(manifest["package_versions"].items())]
    lines += ["", "| Platform | Source packaging run | Engine commit |", "| --- | --- | --- |"]
    for source in manifest["platforms"]:
        url = f"https://github.com/{manifest['repository']}/actions/runs/{source['run_id']}"
        lines.append(f"| `{source['target']}` | [{source['run_id']}]({url}) | `{source['seekdb_commit']}` |")
    lines += ["", "Packages:", ""]
    lines += [f"- `{name}`" for name in sorted({name for source in manifest["platforms"] for name in source["packages"]})]
    lines += ["", "All packages passed their platform smoke tests. See SHA256SUMS for asset checksums.", ""]
    return "\n".join(lines)


def collect(repository, run_ids, assets):
    """Collect finished distributions and verify their source and smoke reports."""
    assets.mkdir(parents=True)
    platforms = []
    versions = {}
    identity = None
    known_artifacts = {"bindings-" + target["id"]: target["id"] for target in TARGETS}
    for run_id in sorted(set(run_ids)):
        run = api(f"repos/{repository}/actions/runs/{run_id}")
        if (run["status"], run["conclusion"], run["event"], run["path"]) != (
                "completed", "success", "workflow_dispatch", ".github/workflows/package.yml"):
            raise ValueError(f"Run {run_id} must be a successful completed Package bindings dispatch")
        if run["repository"]["full_name"] != repository or run["head_repository"]["full_name"] != repository:
            raise ValueError("Choose packaging runs from this repository")
        artifacts = api(f"repos/{repository}/actions/runs/{run_id}/artifacts?per_page=100")["artifacts"]
        selected = [item for item in artifacts if item["name"] in known_artifacts]
        if not selected:
            raise ValueError(f"Run {run_id} has no bindings artifacts")
        for artifact in sorted(selected, key=lambda item: item["name"]):
            platform = known_artifacts[artifact["name"]]
            if artifact["expired"]:
                raise ValueError("Artifact expired: " + artifact["name"])
            if any(item["platform"] == platform for item in platforms):
                raise ValueError("Choose only one packaging run per platform; use component=all to combine bindings")
            with tempfile.TemporaryDirectory() as temporary:
                downloaded = Path(temporary) / "artifact"
                download_artifact(repository, artifact["id"], downloaded)
                metadata, package_versions, components, payloads = verify_bundle(downloaded, platform, run["head_sha"])
                current = (metadata["bindings_commit"], metadata["engine_version"])
                if identity is not None and current != identity:
                    raise ValueError("Packaging runs use different bindings commits or engine versions")
                identity = current
                for name, value in package_versions.items():
                    if name in versions and versions[name] != value:
                        raise ValueError("Packaging runs use different " + name + " package versions")
                    versions[name] = value
                for path in downloaded.iterdir():
                    destination = assets / path.name
                    if destination.exists():
                        # Checksum contents were validated above; Windows may use CRLF.
                        identical = (path.read_text().split() == destination.read_text().split()
                                     if path.name.endswith(".sha256") else digest(path) == digest(destination))
                        if not identical:
                            raise ValueError("Packaging runs contain conflicting files: " + path.name)
                        continue
                    with path.open("rb") as source, destination.open("xb") as output:
                        shutil.copyfileobj(source, output)
                platforms.append({"platform": platform, "target": metadata["target"], "run_id": run_id,
                    "artifact_id": artifact["id"], "seekdb_commit": metadata["seekdb_commit"],
                    "built_at": metadata["built_at"], "components": components, "packages": sorted(payloads)})
    if identity is None:
        raise ValueError("Provide at least one successful packaging run")
    return {"schema_version": 1, "repository": repository, "bindings_commit": identity[0],
            "engine_version": identity[1], "package_versions": versions,
            "platforms": sorted(platforms, key=lambda item: item["platform"]),
            "files": [{"name": path.name, "sha256": digest(path), "size": path.stat().st_size}
                      for path in sorted(assets.iterdir())]}


def prepare(repository, run_ids, tag, destination):
    validate_tag(tag)
    assets = destination / "assets"
    manifest = collect(repository, run_ids, assets)
    check_tag(repository, tag, manifest["bindings_commit"])
    manifest["tag"] = tag
    notes = release_notes(manifest)
    (destination / "release-notes.md").write_text(notes, encoding="utf-8")
    manifest["notes_sha256"] = hashlib.sha256(notes.encode()).hexdigest()
    write_json(assets / "release-manifest.json", manifest)
    manifest_path = assets / "release-manifest.json"
    (assets / "release-manifest.json.sha256").write_text(digest(manifest_path) + "  " + manifest_path.name + "\n")
    (assets / "SHA256SUMS").write_text("".join(digest(path) + "  " + path.name + "\n" for path in sorted(assets.iterdir())))
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
            summary.write(notes)
    print(f"Verified {len(manifest['platforms'])} platforms and {sum(len(item['packages']) for item in manifest['platforms'])} packages", flush=True)
    return manifest


def load_prepared(repository, destination):
    assets = destination / "assets"
    expected = {}
    for line in (assets / "SHA256SUMS").read_text().splitlines():
        checksum, name = line.split()
        safe_name(name)
        if name == "SHA256SUMS" or name in expected or not re.fullmatch("[0-9a-f]{64}", checksum):
            raise ValueError("Invalid release checksum list")
        expected[name] = checksum
    expected["SHA256SUMS"] = digest(assets / "SHA256SUMS")
    if set(expected) != {path.name for path in assets.iterdir()}:
        raise ValueError("Prepared release assets differ from SHA256SUMS")
    for name, checksum in expected.items():
        path = assets / name
        if path.is_symlink() or not path.is_file() or digest(path) != checksum:
            raise ValueError("Prepared release checksum mismatch: " + name)
    manifest = json.loads((assets / "release-manifest.json").read_text())
    if manifest["repository"] != repository:
        raise ValueError("Prepared release belongs to a different repository")
    if digest(destination / "release-notes.md") != manifest["notes_sha256"]:
        raise ValueError("Prepared release notes checksum mismatch")
    validate_tag(manifest["tag"])
    return manifest, expected


def remote_assets(repository, release_id, expected):
    existing = api(f"repos/{repository}/releases/{release_id}/assets?per_page=100")
    partial = []
    completed = set()
    for asset in existing:
        name = asset["name"]
        if name not in expected:
            raise ValueError("Release already contains an unexpected asset: " + name)
        if asset["state"] == "starter":
            partial.append(asset["id"])
        elif asset["state"] == "uploaded" and asset.get("digest") == "sha256:" + expected[name]:
            completed.add(name)
        else:
            raise ValueError("Release asset conflicts with the prepared file: " + name)
    return sorted(set(expected) - completed), partial


def find_release(repository, tag):
    release = api(f"repos/{repository}/releases/tags/{quote(tag, safe='')}", missing_ok=True)
    if release is not None:
        return release
    # The tag endpoint returns published releases; pending draft tags need the list endpoint.
    releases = api(f"repos/{repository}/releases?per_page=100")
    matches = [item for item in releases if item["tag_name"] == tag]
    if len(matches) > 1:
        raise ValueError("Multiple draft releases use this tag")
    if not matches and len(releases) == 100:
        raise ValueError("No matching draft among the first 100 releases; locate or create the draft manually")
    return matches[0] if matches else None


def publish(repository, mode, destination):
    manifest, expected = load_prepared(repository, destination)
    tag = manifest["tag"]
    commit = manifest["bindings_commit"]
    tag_exists = check_tag(repository, tag, commit)
    release = find_release(repository, tag)
    if release is not None and not tag_exists:
        target = api(f"repos/{repository}/commits/{quote(release['target_commitish'], safe='')}")
        if target["sha"] != commit:
            raise ValueError("Existing release targets a different bindings commit")
    if release is not None:
        missing, partial = remote_assets(repository, release["id"], expected)
        if not release["draft"]:
            if missing or partial or mode == "draft" or release["prerelease"] != (mode == "prerelease"):
                raise ValueError("Published release differs; existing published assets will not be overwritten")
            print(release["html_url"], flush=True)
            return release["html_url"]
    ensure_tag(repository, tag, commit)
    if release is None:
        gh("release", "create", tag, "--repo", repository, "--target", commit, "--title", "seekdb bindings " + tag,
           "--notes-file", destination / "release-notes.md", "--draft", "--latest=false", "--verify-tag")
        release = find_release(repository, tag)
        missing, partial = remote_assets(repository, release["id"], expected)
    # Only unfinished uploads are replaced; completed conflicting assets fail above.
    for asset_id in partial:
        api(f"repos/{repository}/releases/assets/{asset_id}", method="DELETE")
    if missing:
        gh("release", "upload", tag, *[destination / "assets" / name for name in missing], "--repo", repository)
    remaining, partial = remote_assets(repository, release["id"], expected)
    if remaining or partial:
        raise ValueError("Release upload is incomplete; leave the release as a draft and retry")
    ensure_tag(repository, tag, commit)
    gh("release", "edit", tag, "--repo", repository, "--target", commit,
       "--title", "seekdb bindings " + tag, "--notes-file", destination / "release-notes.md",
       "--draft=" + str(mode == "draft").lower(), "--prerelease=" + str(mode == "prerelease").lower(),
       "--latest=" + str(mode == "release").lower(), "--verify-tag")
    release = find_release(repository, tag)
    print(release["html_url"], flush=True)
    return release["html_url"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "publish"))
    parser.add_argument("--directory", type=Path, default=ROOT / "build/release")
    arguments = parser.parse_args()
    repository = os.environ["GITHUB_REPOSITORY"]
    if arguments.command == "prepare":
        run_ids = [int(value) for value in re.split(r"[\s,]+", os.environ["RELEASE_RUN_IDS"].strip()) if value]
        prepare(repository, run_ids, os.environ["RELEASE_TAG"], arguments.directory)
    else:
        mode = os.environ["RELEASE_MODE"]
        if mode not in ("draft", "prerelease", "release"):
            raise ValueError("Choose draft, prerelease or release mode")
        url = publish(repository, mode, arguments.directory)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
                summary.write(f"[GitHub Release]({url}) ({mode})\n")


if __name__ == "__main__":
    main()
