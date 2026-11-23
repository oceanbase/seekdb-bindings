#!/usr/bin/env python3
"""Exercise the real macOS dependency bundler and relocated binding runtimes."""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import package


@unittest.skipUnless(sys.platform == "darwin", "Requires macOS compiler and dyld")
class MacOSRuntimeTest(unittest.TestCase):
    def test_bundled_dependencies_survive_flattening_and_relocation(self):
        import resource

        with tempfile.TemporaryDirectory(prefix="seekdb-macos-runtime-") as temporary:
            root = Path(temporary)
            build = root / "build"
            build.mkdir()
            (build / "leaf.c").write_text("int seekdb_test_leaf(void) { return 42; }\n")
            (build / "middle.c").write_text(
                "extern int seekdb_test_leaf(void); int seekdb_test_middle(void) { return seekdb_test_leaf(); }\n")
            (build / "main.c").write_text(
                '#include <stdio.h>\nextern int seekdb_test_middle(void); '
                'int main(void) { printf("%d\\n", seekdb_test_middle()); return 0; }\n')
            leaf = build / "libseekdb_test_leaf.dylib"
            middle = build / "libseekdb_test_middle.dylib"
            executable = build / "seekdb"
            compiler = ["xcrun", "clang", "-mmacosx-version-min=15.0", "-Wl,-headerpad_max_install_names"]
            subprocess.run([*compiler, "-dynamiclib", build / "leaf.c", "-install_name", leaf, "-o", leaf], check=True)
            subprocess.run([*compiler, "-dynamiclib", build / "middle.c", leaf,
                            "-install_name", middle, "-o", middle], check=True)
            subprocess.run([*compiler, build / "main.c", middle, "-o", executable], check=True)

            distribution, _ = package.packaging_tools()
            bundler = Path(distribution.__file__).parent / "pkg/bundle_dylibs.py"
            spec = importlib.util.spec_from_file_location("seekdb_fixture_bundler", bundler)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            engine = root / "engine"
            (engine / "bin").mkdir(parents=True)
            shutil.copy2(executable, engine / "bin/seekdb")
            module.bundle(engine, executable)
            shutil.rmtree(build)
            environment = {key: value for key, value in os.environ.items() if not key.startswith("DYLD_")}
            self.assertEqual(subprocess.check_output([engine / "bin/seekdb"], env=environment, text=True), "42\n")
            original = subprocess.check_output(["otool", "-L", engine / "bin/seekdb"], text=True)
            self.assertIn("@executable_path/../lib/seekdb/" + middle.name, original)

            prefix = root / "sdk"
            package.stage_engine(engine, prefix)
            with patch.dict(os.environ, {"PACKAGE_OS": "macos"}):
                package.repair_native(prefix / "lib")
            imports = subprocess.check_output(["otool", "-L", prefix / "lib/seekdb"], text=True)
            for system in ("/usr/lib/", "/System/Library/"):
                original_system = [line.strip() for line in original.splitlines() if line.strip().startswith(system)]
                repaired_system = [line.strip() for line in imports.splitlines() if line.strip().startswith(system)]
                self.assertEqual(original_system, repaired_system)

            # CLI fixtures must not leave core files if a regression aborts dyld.
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            for name in ("sdk/lib", "wheel/pylibseekdb/bin", "npm/dist/native/bin", "java/lib"):
                runtime = root / name
                if runtime != prefix / "lib":
                    shutil.copytree(prefix / "lib", runtime)
                result = subprocess.run([runtime / "seekdb"], cwd=root, env=environment, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "42\n")
            self.assertIn("@loader_path/" + middle.name, imports)
            self.assertNotIn("@executable_path/../lib/seekdb/", imports)


if __name__ == "__main__":
    unittest.main()
