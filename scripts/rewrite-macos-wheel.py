#!/usr/bin/env python3
"""Replace dynamic RE2/Abseil in a repaired macOS wheel with one merged RE2."""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from typing import NoReturn
import zipfile


def fail(message: str) -> NoReturn:
    raise SystemExit(f"error: {message}")


def otool_dependencies(path: Path) -> list[str] | None:
    result = subprocess.run(
        ["otool", "-L", str(path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if result.returncode != 0:
        return None
    return [line.strip().split(" ", 1)[0] for line in result.stdout.splitlines()[1:]]


def native_files(package_dir: Path) -> list[Path]:
    candidates = [path for path in package_dir.rglob("*") if path.is_file()]
    return [path for path in candidates if otool_dependencies(path) is not None]


def rebuild_record(root: Path) -> None:
    records = list(root.glob("*.dist-info/RECORD"))
    if len(records) != 1:
        fail(f"expected one dist-info/RECORD, found {len(records)}")
    record = records[0]
    record_relative = record.relative_to(root).as_posix()

    files = sorted(path for path in root.rglob("*") if path.is_file())
    with record.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        for path in files:
            relative = path.relative_to(root).as_posix()
            if relative == record_relative:
                writer.writerow((relative, "", ""))
                continue
            data = path.read_bytes()
            digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
            writer.writerow((relative, f"sha256={digest}", str(len(data))))


def repack_wheel(root: Path, wheel: Path) -> None:
    temporary_wheel = wheel.with_name(f".{wheel.name}.tmp")
    try:
        with zipfile.ZipFile(
            temporary_wheel,
            "w",
            compression=zipfile.ZIP_DEFLATED,
            compresslevel=9,
        ) as archive:
            for path in sorted(item for item in root.rglob("*") if item.is_file()):
                archive.write(path, path.relative_to(root).as_posix())
        os.replace(temporary_wheel, wheel)
    finally:
        temporary_wheel.unlink(missing_ok=True)


def rewrite(wheel: Path, merged_re2: Path, max_bundled_dylibs: int) -> None:
    if not wheel.is_file():
        fail(f"wheel not found: {wheel}")
    if not merged_re2.is_file():
        fail(f"merged RE2 not found: {merged_re2}")

    merged_dependencies = otool_dependencies(merged_re2)
    if merged_dependencies is None:
        fail(f"merged RE2 is not a Mach-O file: {merged_re2}")
    if any("libabsl_" in dependency for dependency in merged_dependencies):
        fail("merged RE2 still references dynamic Abseil libraries")
    bad_merged_dependencies = [
        dependency
        for dependency in merged_dependencies
        if dependency.startswith("/")
        and not dependency.startswith("/usr/lib/")
        and not dependency.startswith("/System/Library/")
    ]
    if bad_merged_dependencies:
        fail(f"merged RE2 has non-system absolute dependencies: {bad_merged_dependencies}")

    with tempfile.TemporaryDirectory(prefix="pylibseekdb-wheel-rewrite-") as temporary:
        root = Path(temporary)
        with zipfile.ZipFile(wheel) as archive:
            archive.extractall(root)
            # zipfile.extractall() does not restore Unix permission bits. Keep
            # seekdb executable and preserve the original modes of all entries.
            for member in archive.infolist():
                mode = (member.external_attr >> 16) & 0o7777
                extracted = root / member.filename
                if mode and extracted.exists():
                    extracted.chmod(mode)

        package_dir = root / "pylibseekdb"
        dylib_dir = package_dir / ".dylibs"
        if not dylib_dir.is_dir():
            fail("wheel does not contain pylibseekdb/.dylibs")
        seekdb_binary = package_dir / "seekdb"
        if not seekdb_binary.is_file() or not os.access(seekdb_binary, os.X_OK):
            fail("wheel seekdb binary is not executable")

        re2_files = sorted(dylib_dir.glob("libre2*.dylib"))
        abseil_files = sorted(dylib_dir.glob("libabsl_*.dylib"))
        if not abseil_files:
            after_dylibs = len(list(dylib_dir.glob("*.dylib")))
            if after_dylibs > max_bundled_dylibs:
                fail(
                    f"bundled dylib count {after_dylibs} exceeds limit "
                    f"{max_bundled_dylibs}"
                )
            print(f"no dynamic Abseil closure found; bundled dylibs: {after_dylibs}")
            return
        if len(re2_files) != 1:
            fail(f"expected one bundled RE2 dylib, found {len(re2_files)}")

        re2_file = re2_files[0]
        consumers: list[Path] = []
        for path in native_files(package_dir):
            if path in abseil_files:
                continue
            dependencies = otool_dependencies(path) or []
            if any("libabsl_" in dependency for dependency in dependencies):
                consumers.append(path)
        if consumers != [re2_file]:
            relative_consumers = [str(path.relative_to(root)) for path in consumers]
            fail(f"dynamic Abseil has unexpected non-RE2 consumers: {relative_consumers}")

        before_dylibs = len(list(dylib_dir.glob("*.dylib")))
        shutil.copy2(merged_re2, re2_file)
        subprocess.run(
            ["install_name_tool", "-id", f"@loader_path/{re2_file.name}", str(re2_file)],
            check=True,
        )
        subprocess.run(["codesign", "--force", "--sign", "-", str(re2_file)], check=True)

        for path in abseil_files:
            path.unlink()

        remaining_abseil_references: list[str] = []
        for path in native_files(package_dir):
            dependencies = otool_dependencies(path) or []
            if any("libabsl_" in dependency for dependency in dependencies):
                remaining_abseil_references.append(str(path.relative_to(root)))
        if remaining_abseil_references:
            fail(f"wheel still references dynamic Abseil: {remaining_abseil_references}")

        after_dylibs = len(list(dylib_dir.glob("*.dylib")))
        if after_dylibs > max_bundled_dylibs:
            fail(
                f"bundled dylib count {after_dylibs} exceeds limit {max_bundled_dylibs}"
            )

        rebuild_record(root)
        repack_wheel(root, wheel)
        print(
            f"collapsed RE2/Abseil: bundled dylibs {before_dylibs} -> {after_dylibs}; "
            f"removed {len(abseil_files)} Abseil dylibs"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wheel", type=Path)
    parser.add_argument("merged_re2", type=Path)
    parser.add_argument("--max-bundled-dylibs", type=int, default=20)
    arguments = parser.parse_args()
    rewrite(arguments.wheel.resolve(), arguments.merged_re2.resolve(), arguments.max_bundled_dylibs)


if __name__ == "__main__":
    main()
