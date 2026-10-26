#!/usr/bin/env python3
"""Build a Cargo application and stage its complete SeekDB native runtime."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def package(args):
    output = args.output.resolve()
    if output.exists():
        raise ValueError(f"output already exists: {output}; choose a new directory")
    kind, name = ("example", args.example) if args.example else ("bin", args.bin)
    command = [
        "cargo", "build", "--locked", "--manifest-path", str(args.manifest_path),
        "--profile", args.profile, f"--{kind}", name,
        "--message-format=json-render-diagnostics",
    ]
    if args.target:
        command.extend(["--target", args.target])
    # Cargo handles its standard environment/configuration, including target
    # directory selection. Discover actual paths from its JSON output.
    result = subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE)
    executable = None
    native_directories = set()
    for line in result.stdout.splitlines():
        event = json.loads(line)
        if event["reason"] == "compiler-artifact":
            target = event["target"]
            if target["name"] == name and kind in target["kind"] and event.get("executable"):
                executable = Path(event["executable"])
        elif event["reason"] == "build-script-executed":
            for entry in event.get("linked_paths", []):
                path = Path(entry.removeprefix("native="))
                if (path / "seekdb").is_file() and any(
                    (path / library).is_file()
                    for library in ("libseekdb.so", "libseekdb.dylib")
                ):
                    native_directories.add(path.resolve())
    if executable is None:
        raise ValueError(f"Cargo did not report an executable for {kind} {name}")
    if len(native_directories) != 1:
        raise ValueError(
            f"expected one SeekDB native runtime, found {len(native_directories)}"
        )
    native = native_directories.pop()
    if output == native or native in output.parents:
        raise ValueError("output must be outside the native runtime directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f".{output.name}-", dir=output.parent) as temporary:
        staging = Path(temporary) / "bundle"
        staging.mkdir()
        shutil.copy2(executable, staging / executable.name)
        # Preserve hidden directories, symlinks, permissions, and the complete
        # dependency layout; the native bundle already carries relative RPATHs.
        shutil.copytree(native, staging / "lib", symlinks=True)
        os.rename(staging, output)
    print(f"Application bundle: {output}")
    print(f"Executable: {output / executable.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest-path", type=Path, required=True)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--bin")
    target.add_argument("--example")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", default="release")
    parser.add_argument("--target", help="Cargo target triple (requires a supported native bundle)")
    try:
        package(parser.parse_args())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
