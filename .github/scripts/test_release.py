#!/usr/bin/env python3
"""Verify artifact integrity and the release lifecycle without writing to GitHub."""

import copy
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import release

REPOSITORY = "oceanbase/seekdb-bindings"
COMMIT = "a" * 40
TAG = "v1.4.0"


def bundle(directory, platform="linux-x86_64", components=("sdk",)):
    directory.mkdir()
    suffix = {"linux-x86_64": "glibc2.28", "macos-arm64": "min15.0", "android-arm64": "api28"}[platform]
    target = platform + "-" + suffix
    versions = dict.fromkeys(("sdk", "python", "node", "java", "android"), "1.4.0")
    metadata = {"target": target, "bindings_commit": COMMIT, "engine_version": "1.4.0.0",
                "seekdb_commit": "b" * 40, "built_at": "20261123T000000Z", "package_versions": versions}
    names = {"sdk": f"libseekdb-1.4.0-{target}-20261123T000000Z-sdbbbbb-bndaaaa.tar.gz",
             "android-aar": "seekdb-android-1.4.0-arm64-api28-20261123T000000Z-sdbbbbb-bndaaaa.aar"}
    for component in components:
        (directory / names[component]).write_bytes((component + " payload").encode())
    release.write_json(directory / (target + "-provenance.json"), metadata)
    release.write_json(directory / (target + "-smoke.json"),
                       {"target": target, "source_commit": COMMIT, "status": "passed", "packages": list(components)})
    rehash(directory)
    return metadata


def rehash(directory):
    for path in directory.iterdir():
        if not path.name.endswith(".sha256"):
            path.with_name(path.name + ".sha256").write_text(release.digest(path) + "  " + path.name + "\n")


class FakeGitHub:
    def __init__(self):
        self.runs = {}
        self.artifacts = {}
        self.directories = {}
        self.release = None
        self.assets = {}
        self.tag = None
        self.tag_objects = {}
        self.writes = []
        self.interrupt_upload = False
        self.corrupt_upload = False

    def add(self, run_id, directory, platform="linux-x86_64"):
        self.runs[run_id] = {"status": "completed", "conclusion": "success", "event": "workflow_dispatch",
            "path": ".github/workflows/package.yml", "head_sha": COMMIT,
            "repository": {"full_name": REPOSITORY}, "head_repository": {"full_name": REPOSITORY}}
        artifact_id = run_id * 10
        self.artifacts[run_id] = [{"id": artifact_id, "name": "bindings-" + platform, "expired": False}]
        self.directories[artifact_id] = directory

    def api(self, path, missing_ok=False, method="GET"):
        prefix = f"repos/{REPOSITORY}/"
        assert path.startswith(prefix), path
        relative = path.removeprefix(prefix)
        if relative.startswith("actions/runs/"):
            run_id = int(relative.split("/")[2])
            if relative.endswith("/artifacts?per_page=100"):
                return {"artifacts": self.artifacts[run_id]}
            return self.runs[run_id]
        if relative.startswith("git/ref/tags/"):
            return {"object": self.tag} if self.tag else None
        if relative.startswith("git/tags/"):
            return {"object": self.tag_objects[relative.split("/")[-1]]}
        if relative.startswith("commits/"):
            return {"sha": relative.removeprefix("commits/")}
        if relative.startswith("releases/tags/"):
            # GitHub's tag lookup does not find pending drafts.
            return self.release if self.release and not self.release["draft"] else None
        if relative == "releases?per_page=100":
            return [self.release] if self.release else []
        if relative == "releases/1/assets?per_page=100":
            return copy.deepcopy(list(self.assets.values()))
        if relative.startswith("releases/assets/") and method == "DELETE":
            asset_id = int(relative.split("/")[-1])
            name = next(name for name, asset in self.assets.items() if asset["id"] == asset_id)
            self.writes.append(("delete", name))
            del self.assets[name]
            return None
        raise AssertionError(path)

    def download(self, repository, artifact_id, destination):
        self.assert_repository(repository)
        shutil.copytree(self.directories[artifact_id], destination)

    def assert_repository(self, repository):
        assert repository == REPOSITORY

    def gh(self, *arguments, **kwargs):
        action = arguments[1]
        self.writes.append((action, arguments))
        if action == "create":
            self.release = {"id": 1, "draft": True, "prerelease": False, "tag_name": TAG,
                            "target_commitish": arguments[arguments.index("--target") + 1],
                            "html_url": f"https://github.com/{REPOSITORY}/releases/tag/{TAG}"}
        elif action == "upload":
            for path in arguments[3:arguments.index("--repo")]:
                name = path.name
                self.assets[name] = {"id": len(self.assets) + 10, "name": name, "state": "uploaded",
                                     "digest": "sha256:" + release.digest(path)}
                if self.corrupt_upload:
                    self.assets[name]["digest"] = "sha256:" + "0" * 64
                if self.interrupt_upload:
                    self.interrupt_upload = False
                    raise RuntimeError("interrupted upload")
        elif action == "edit":
            self.release["draft"] = "--draft=true" in arguments
            self.release["prerelease"] = "--prerelease=true" in arguments
            if not self.release["draft"]:
                self.tag = {"type": "commit", "sha": COMMIT}
        else:
            raise AssertionError(arguments)


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        output = redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source"
        self.metadata = bundle(self.source)
        self.remote = FakeGitHub()
        self.remote.add(1, self.source)
        for name, value in (("api", self.remote.api), ("gh", self.remote.gh), ("download_artifact", self.remote.download)):
            patcher = patch.object(release, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.destination = self.root / "prepared"

    def prepare(self, runs=(1,), destination=None):
        return release.prepare(REPOSITORY, runs, TAG, destination or self.destination)

    def report(self, **updates):
        path, = self.source.glob("*-smoke.json")
        data = json.loads(path.read_text())
        data.update(updates)
        release.write_json(path, data)
        rehash(self.source)

    def test_prepare_and_check_do_not_write_to_github(self):
        manifest = self.prepare()
        self.assertEqual(manifest["bindings_commit"], COMMIT)
        self.assertEqual(manifest["package_versions"], {"sdk": "1.4.0"})
        self.assertEqual(self.remote.writes, [])
        release.load_prepared(REPOSITORY, self.destination)

    def test_failed_run_rejected_before_downloading(self):
        self.remote.runs[1]["conclusion"] = "failure"
        with self.assertRaisesRegex(ValueError, "successful completed"):
            self.prepare()

    def test_other_workflow_rejected(self):
        self.remote.runs[1]["path"] = ".github/workflows/android-smoke.yml"
        with self.assertRaisesRegex(ValueError, "Package bindings"):
            self.prepare()

    def test_expired_artifact_rejected(self):
        self.remote.artifacts[1][0]["expired"] = True
        with self.assertRaisesRegex(ValueError, "expired"):
            self.prepare()

    def test_tampered_package_rejected(self):
        path, = self.source.glob("*.tar.gz")
        path.write_bytes(b"modified")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.prepare()

    def test_android_provisional_report_rejected(self):
        shutil.rmtree(self.source)
        bundle(self.source, "android-arm64", ("sdk", "android-aar"))
        self.remote.add(1, self.source, "android-arm64")
        self.report(status="not_run")
        with self.assertRaisesRegex(ValueError, "passed smoke report"):
            self.prepare()

    def test_report_must_cover_packaged_components(self):
        self.report(packages=["sdk", "python-cp311"])
        with self.assertRaisesRegex(ValueError, "Both Python"):
            self.prepare()

    def test_report_and_run_commits_must_match(self):
        self.report(source_commit="c" * 40)
        with self.assertRaisesRegex(ValueError, "source commit"):
            self.prepare()

    def test_multiple_runs_are_deterministic(self):
        mac = self.root / "mac"
        metadata = bundle(mac, "macos-arm64")
        metadata["seekdb_commit"] = "c" * 40
        release.write_json(mac / (metadata["target"] + "-provenance.json"), metadata)
        rehash(mac)
        self.remote.add(2, mac, "macos-arm64")
        self.prepare((2, 1))
        other = self.root / "other"
        self.prepare((1, 2), other)
        for path in self.destination.rglob("*"):
            if path.is_file():
                self.assertEqual(path.read_bytes(), (other / path.relative_to(self.destination)).read_bytes())

    def test_mixed_source_runs_rejected(self):
        mac = self.root / "mac"
        metadata = bundle(mac, "macos-arm64")
        metadata["bindings_commit"] = "c" * 40
        release.write_json(mac / (metadata["target"] + "-provenance.json"), metadata)
        report_path, = mac.glob("*-smoke.json")
        report = json.loads(report_path.read_text())
        report["source_commit"] = "c" * 40
        release.write_json(report_path, report)
        rehash(mac)
        self.remote.add(2, mac, "macos-arm64")
        self.remote.runs[2]["head_sha"] = "c" * 40
        with self.assertRaisesRegex(ValueError, "different bindings commits"):
            self.prepare((1, 2))

    def test_duplicate_platform_rejected(self):
        self.remote.add(2, self.source)
        with self.assertRaisesRegex(ValueError, "one packaging run per platform"):
            self.prepare((1, 2))

    def test_existing_wrong_tag_rejected_without_writes(self):
        self.remote.tag = {"type": "commit", "sha": "c" * 40}
        with self.assertRaisesRegex(ValueError, "different bindings commit"):
            self.prepare()
        self.assertEqual(self.remote.writes, [])

    def test_annotated_tag_is_resolved_to_commit(self):
        self.remote.tag = {"type": "tag", "sha": "d" * 40}
        self.remote.tag_objects["d" * 40] = {"type": "commit", "sha": COMMIT}
        self.prepare()

    def test_local_assets_tampered_after_prepare_rejected(self):
        self.prepare()
        path, = (self.destination / "assets").glob("*.tar.gz")
        path.write_bytes(b"changed after validation")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            release.publish(REPOSITORY, "release", self.destination)
        self.assertEqual(self.remote.writes, [])

    def test_interrupted_draft_resumes_without_overwriting_completed_assets(self):
        self.prepare()
        self.remote.interrupt_upload = True
        with self.assertRaisesRegex(RuntimeError, "interrupted upload"):
            release.publish(REPOSITORY, "release", self.destination)
        self.assertTrue(self.remote.release["draft"])
        completed = set(self.remote.assets)
        self.remote.writes.clear()
        release.publish(REPOSITORY, "release", self.destination)
        uploaded = next(arguments for action, arguments in self.remote.writes if action == "upload")
        self.assertTrue(completed.isdisjoint(path.name for path in uploaded[3:uploaded.index("--repo")]))
        self.assertFalse(self.remote.release["draft"])

    def test_draft_remains_unpublished(self):
        self.prepare()
        release.publish(REPOSITORY, "draft", self.destination)
        self.assertTrue(self.remote.release["draft"])
        self.assertEqual(self.remote.release["target_commitish"], COMMIT)

    def test_prerelease_flag_set_after_uploads(self):
        self.prepare()
        release.publish(REPOSITORY, "prerelease", self.destination)
        self.assertFalse(self.remote.release["draft"])
        self.assertTrue(self.remote.release["prerelease"])
        self.assertEqual(self.remote.writes[-1][0], "edit")

    def test_upload_digest_mismatch_does_not_publish(self):
        self.prepare()
        self.remote.corrupt_upload = True
        with self.assertRaisesRegex(ValueError, "conflicts"):
            release.publish(REPOSITORY, "release", self.destination)
        self.assertTrue(self.remote.release["draft"])
        self.assertNotIn("edit", [action for action, _ in self.remote.writes])

    def test_published_release_retry_is_read_only(self):
        self.prepare()
        release.publish(REPOSITORY, "release", self.destination)
        self.remote.writes.clear()
        release.publish(REPOSITORY, "release", self.destination)
        self.assertEqual(self.remote.writes, [])

    def test_conflicting_remote_asset_is_not_overwritten(self):
        self.prepare()
        release.publish(REPOSITORY, "draft", self.destination)
        next(iter(self.remote.assets.values()))["digest"] = "sha256:" + "0" * 64
        self.remote.writes.clear()
        with self.assertRaisesRegex(ValueError, "conflicts"):
            release.publish(REPOSITORY, "release", self.destination)
        self.assertEqual(self.remote.writes, [])


class ArtifactZipTest(unittest.TestCase):
    def test_path_traversal_rejected_without_writing_outside_destination(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "input.zip"
            with zipfile.ZipFile(archive, "w") as stream:
                stream.writestr("../outside.txt", "bad")
            def download(*arguments, stdout):
                stdout.write(archive.read_bytes())
            with patch.object(release, "gh", download):
                with self.assertRaisesRegex(ValueError, "filename"):
                    release.download_artifact(REPOSITORY, 1, root / "output")
            self.assertFalse((root / "outside.txt").exists())


if __name__ == "__main__":
    unittest.main()
