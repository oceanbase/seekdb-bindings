import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import package


class PackageTest(unittest.TestCase):
    def test_cargo_paths_and_complete_native_layout_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "custom-target" / "custom-profile" / "my-app"
            executable.parent.mkdir(parents=True)
            executable.write_text("application")
            executable.chmod(0o751)
            native = root / "native"
            (native / ".libs").mkdir(parents=True)
            for name in ("libseekdb.so", "seekdb"):
                (native / name).write_text(name)
            (native / "seekdb").chmod(0o751)
            (native / ".libs" / "libaio-real.so.1").write_text("dependency")
            (native / ".libs" / "libaio.so.1").symlink_to("libaio-real.so.1")
            events = [
                {
                    "reason": "compiler-artifact",
                    "target": {"name": "my-app", "kind": ["bin"]},
                    "executable": str(executable),
                },
                {"reason": "build-script-executed", "linked_paths": [f"native={native}"]},
            ]
            result = subprocess.CompletedProcess(
                [], 0, stdout="\n".join(json.dumps(event) for event in events)
            )
            args = argparse.Namespace(
                manifest_path=root / "Cargo.toml", bin="my-app", example=None,
                profile="custom-profile", target=None, output=root / "dist",
            )
            with patch("package.subprocess.run", return_value=result):
                package.package(args)
            self.assertEqual((args.output / "my-app").stat().st_mode & 0o777, 0o751)
            self.assertEqual((args.output / "lib" / "seekdb").stat().st_mode & 0o777, 0o751)
            dependency = args.output / "lib" / ".libs" / "libaio.so.1"
            self.assertTrue(dependency.is_symlink())
            self.assertEqual(dependency.read_text(), "dependency")
            self.assertTrue(executable.exists())
            self.assertTrue((native / "libseekdb.so").exists())
            with self.assertRaisesRegex(ValueError, "output already exists"):
                package.package(args)


if __name__ == "__main__":
    unittest.main()
