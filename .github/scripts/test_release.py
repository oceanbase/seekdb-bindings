import unittest
from pathlib import Path
from unittest.mock import call, patch

import release


def uploaded(asset_id, name, checksum):
    return {"id": asset_id, "name": name, "state": "uploaded", "digest": "sha256:" + checksum}


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.repository = "oceanbase/seekdb-bindings"
        self.destination = Path("build/release")
        self.tag = "v1.4.1"
        self.sdk = "libseekdb-1.4.1-linux-x86_64.tar.gz"
        self.node = "oceanbase-seekdb-node-1.4.1-repack.1.tgz"
        self.manifest = {
            "tag": self.tag,
            "bindings_commit": "bindings-commit",
            "platforms": [{"packages": [self.sdk, self.node]}, {"packages": [self.node]}],
        }
        self.checksums = {
            self.sdk: "a" * 64,
            self.node: "b" * 64,
            self.sdk + ".sha256": "c" * 64,
            "linux-x86_64-smoke.json": "d" * 64,
            "release-manifest.json": "e" * 64,
            "SHA256SUMS": "f" * 64,
        }
        self.package_assets = [
            uploaded(1, self.sdk, self.checksums[self.sdk]),
            uploaded(2, self.node, self.checksums[self.node]),
        ]
        self.legacy_asset = uploaded(3, "release-manifest.json", "0" * 64)
        self.release_path = f"repos/{self.repository}/releases/42/assets?per_page=100"
        self.url = f"https://github.com/{self.repository}/releases/tag/{self.tag}"

    def test_release_notes_link_to_the_engine_release(self):
        manifest = {"tag": "v2.0.0", "engine_version": "1.4.1.0"}
        self.assertEqual(
            release.release_notes(manifest),
            "[seekdb v1.4.1 Release](https://github.com/oceanbase/seekdb/releases/tag/v1.4.1)\n",
        )

    def test_draft_upload_only_includes_packages(self):
        draft = {"id": 42, "draft": True, "html_url": self.url}
        partial_node = {"id": 2, "name": self.node, "state": "starter"}
        with (
            patch.object(release, "load_prepared", return_value=(self.manifest, self.checksums)),
            patch.object(release, "check_tag", return_value=True),
            patch.object(release, "find_release", return_value=draft),
            patch.object(release, "api", side_effect=[
                [self.package_assets[0], partial_node, self.legacy_asset], None, None, self.package_assets,
            ]) as api,
            patch.object(release, "gh") as gh,
        ):
            self.assertEqual(release.publish(self.repository, "release", self.destination), self.url)

        api.assert_has_calls([
            call(self.release_path),
            call(f"repos/{self.repository}/releases/assets/2", method="DELETE"),
            call(f"repos/{self.repository}/releases/assets/3", method="DELETE"),
            call(self.release_path),
        ])
        uploads = [command for command in gh.call_args_list if command.args[:2] == ("release", "upload")]
        self.assertEqual(uploads, [
            call("release", "upload", self.tag, self.destination / "assets" / self.node, "--repo", self.repository),
        ])

    def test_published_release_with_legacy_assets_is_accepted(self):
        published = {"id": 42, "draft": False, "prerelease": False, "html_url": self.url}
        with (
            patch.object(release, "load_prepared", return_value=(self.manifest, self.checksums)),
            patch.object(release, "check_tag", return_value=True),
            patch.object(release, "find_release", return_value=published),
            patch.object(release, "api", return_value=[*self.package_assets, self.legacy_asset]) as api,
            patch.object(release, "gh") as gh,
        ):
            self.assertEqual(release.publish(self.repository, "release", self.destination), self.url)

        api.assert_called_once_with(self.release_path)
        gh.assert_not_called()


if __name__ == "__main__":
    unittest.main()
