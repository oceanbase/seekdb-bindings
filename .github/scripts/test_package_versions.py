#!/usr/bin/env python3
"""Check version selection and the metadata written to package archives."""

import json
import os
from pathlib import Path
import tarfile
import tempfile
import tomllib
import unittest
from unittest.mock import patch

import package


class PackageVersionsTest(unittest.TestCase):
    def setUp(self):
        self.environment = patch.dict(os.environ, {
            **dict.fromkeys(package.VERSION_ENV, ""), "PACKAGE_OS": "linux", "PACKAGE_COMPONENT": "all",
        })
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def test_blank_inputs_keep_source_and_engine_versions(self):
        versions = package.package_versions("1.4.0.0")
        python = tomllib.loads((package.ROOT / "python/pyproject.toml").read_text())
        npm = json.loads((package.ROOT / "typescript/package.json").read_text())
        self.assertEqual(versions["python"], python["project"]["version"])
        self.assertEqual(versions["node"], npm["version"])
        for component in ("sdk", "java", "android"):
            self.assertEqual(versions[component], "1.4.0.0")

    def test_common_version_and_independent_overrides(self):
        os.environ.update(PACKAGE_VERSION="1.4.1", PACKAGE_PYTHON_VERSION="1.4.0.post3",
                          PACKAGE_NODE_VERSION="2.0.0-rc.1", PACKAGE_ANDROID_VERSION="3.0.0")
        self.assertEqual(package.package_versions("1.4.0.0"), {
            "sdk": "1.4.1", "python": "1.4.0.post3", "node": "2.0.0-rc.1",
            "java": "1.4.1", "android": "3.0.0",
        })

    def test_python_prerelease_is_normalized_for_metadata(self):
        os.environ["PACKAGE_VERSION"] = "1.5.0-rc.1"
        versions = package.package_versions("1.4.0.0")
        self.assertEqual(versions["python"], "1.5.0rc1")
        self.assertEqual(versions["node"], "1.5.0-rc.1")

    def test_component_version_does_not_change_other_defaults(self):
        os.environ["PACKAGE_JAVA_VERSION"] = "2.0.0"
        versions = package.package_versions("1.4.0.0")
        self.assertEqual(versions["java"], "2.0.0")
        self.assertEqual(versions["sdk"], "1.4.0.0")
        self.assertEqual(versions["android"], "1.4.0.0")
        self.assertEqual(versions["node"], json.loads((package.ROOT / "typescript/package.json").read_text())["version"])

    def test_python_only_version_is_valid_for_android_sdk(self):
        os.environ.update(PACKAGE_OS="android", PACKAGE_VERSION="1.4.0.post3")
        self.assertEqual(package.package_versions("1.4.0.0")["android"], "1.4.0.post3")

    def test_version_cannot_escape_archive_directory(self):
        os.environ["PACKAGE_SDK_VERSION"] = "1.0/../outside"
        with self.assertRaisesRegex(ValueError, "Invalid sdk package version"):
            package.package_versions("1.4.0.0")

    def test_python_project_restored_after_build_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / "python/pyproject.toml"
            project.parent.mkdir()
            original = b'[project]\nname = "example"\nversion = "0.1.0"\n'
            project.write_bytes(original)
            with patch.object(package, "ROOT", root):
                with self.assertRaisesRegex(RuntimeError, "build failed"):
                    with package.python_project_version("1.4.0.post3"):
                        self.assertEqual(tomllib.loads(project.read_text())["project"]["version"], "1.4.0.post3")
                        raise RuntimeError("build failed")
            self.assertEqual(project.read_bytes(), original)

    def test_python_override_preserves_crlf_checkout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / "python/pyproject.toml"
            project.parent.mkdir()
            original = b'[project]\r\nname = "example"\r\nversion = "0.1.0"\r\n'
            project.write_bytes(original)
            with patch.object(package, "ROOT", root):
                with package.python_project_version("1.4.0.post3"):
                    self.assertEqual(project.read_bytes(), original.replace(b"0.1.0", b"1.4.0.post3"))
                    self.assertEqual(tomllib.loads(project.read_text())["project"]["version"], "1.4.0.post3")
            self.assertEqual(project.read_bytes(), original)

    def test_archives_keep_engine_identity_with_independent_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            metadata = {"engine_version": "1.4.0.0", "package_versions": {"sdk": "2.0.0", "java": "3.0.0"},
                        "target": "linux-x86_64-glibc2.28", "built_at": "20261121T000000Z",
                        "seekdb_commit": "a" * 40, "bindings_commit": "b" * 40}
            with patch.object(package, "DIST", root):
                for kind, component in (("libseekdb", "sdk"), ("seekdb-java", "java")):
                    prefix = root / component
                    prefix.mkdir()
                    archive = package.archive(prefix, kind, metadata, component)
                    self.assertTrue(archive.name.startswith(kind + "-" + metadata["package_versions"][component] + "-"))
                    with tarfile.open(archive) as stream:
                        actual = json.load(stream.extractfile("BUILD-INFO.json"))
                    self.assertEqual(actual["engine_version"], "1.4.0.0")
                    self.assertEqual(actual["package_versions"], metadata["package_versions"])
                    self.assertEqual(actual["seekdb_commit"], "a" * 40)


if __name__ == "__main__":
    unittest.main()
