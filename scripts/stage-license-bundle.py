#!/usr/bin/env python3
"""Stage the bindings layer of the third-party license bundle.

The SDK prefix produced by ``.github/scripts/package.py`` already carries the
engine's license bundle under ``licenses/``.  This script validates the
bindings component corpus in ``licenses/`` and renders the bindings layer next
to it: the project LICENSE and NOTICE, the component inventory, the
redistribution requirements, a checksum manifest, and the original license
texts.
"""

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LICENSES = ROOT / "licenses"
PROJECT = {
    "name": "pylibseekdb",
    "source": "https://github.com/oceanbase/seekdb-bindings",
    "license": "Apache-2.0",
    "license_files": ["LICENSE"],
    "notice_files": ["NOTICE"],
}
REQUIRED_FIELDS = (
    "name",
    "version",
    "source",
    "packaged_as",
    "license",
    "modified",
    "license_files",
    "redistribution",
)


def load_components():
    components = []
    for directory in sorted(LICENSES.iterdir()):
        if not directory.is_dir():
            continue
        metadata = directory / "METADATA.json"
        if not metadata.is_file():
            raise ValueError("component directory %s is missing METADATA.json" % directory)
        component = json.loads(metadata.read_text(encoding="utf-8"))
        for field in REQUIRED_FIELDS:
            if field not in component:
                raise ValueError("component %r is missing %s" % (component.get("name"), field))
        if not component["license_files"]:
            raise ValueError("component %s has no license files" % component["name"])
        for relative in component["license_files"]:
            if not (directory / relative).is_file():
                raise ValueError("component %s references missing license file %s" % (
                    component["name"], relative))
        component["directory"] = directory.name
        components.append(component)
    if not components:
        raise ValueError("no component directories found under %s" % LICENSES)
    return components


def python_package_version(name):
    try:
        import importlib.metadata
        return importlib.metadata.version(name)
    except Exception:
        return None


def engine_inventory(sdk_dir):
    path = Path(sdk_dir) / "licenses" / "THIRD_PARTY_COMPONENTS.json"
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def engine_component_version(sdk_dir, name):
    inventory = engine_inventory(sdk_dir)
    if not inventory:
        return None
    for component in inventory.get("components", []):
        if component.get("name", "").lower() == name.lower():
            return component.get("version")
    return None


def resolve_version(component, sdk_dir):
    """Return (version, notes) after applying version_from and version_check."""
    version = component["version"]
    notes = []
    source = component.get("version_from")
    if source and "python_package" in source:
        resolved = python_package_version(source["python_package"])
        if resolved:
            version = resolved
        else:
            notes.append("%s version not resolvable at build time; using %s" % (
                component["name"], version))
    check = component.get("version_check")
    if check:
        if "engine_component" in check:
            actual = engine_component_version(sdk_dir, check["engine_component"])
            if actual is None:
                notes.append("engine inventory has no %s entry; %s version not cross-checked" % (
                    check["engine_component"], component["name"]))
            elif actual != version:
                raise ValueError("%s version %s does not match the engine component %s (%s)" % (
                    component["name"], version, check["engine_component"], actual))
        else:
            path = ROOT / check["file"]
            if not path.is_file():
                notes.append("version source %s is missing; %s version not cross-checked" % (
                    check["file"], component["name"]))
            else:
                match = re.search(check["pattern"], path.read_text(encoding="utf-8", errors="replace"))
                if not match:
                    raise ValueError("cannot parse %s to verify the %s version" % (
                        check["file"], component["name"]))
                actual = check["format"].format(*match.groups())
                if actual != version:
                    raise ValueError("%s version %s does not match %s (%s)" % (
                        component["name"], version, check["file"], actual))
    return version, notes


def markdown(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


def license_cell(component):
    return "<br>".join("`bindings/%s/%s`" % (component["directory"], relative)
                       for relative in component["license_files"])


def artifact_version(sdk_dir, override):
    if override:
        return override
    path = Path(sdk_dir) / "BUILD-INFO.json"
    if path.is_file():
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            version = data.get("package_versions", {}).get("python")
            if version:
                return version
        except (OSError, ValueError):
            pass
    return "unspecified"


def render_inventory(components, version):
    lines = [
        "# Third-party components bundled by pylibseekdb",
        "",
        "This inventory covers the components added by the bindings layer.",
        "The seekdb engine's own components are listed in",
        "`licenses/THIRD_PARTY_COMPONENTS.md` next to this directory.",
        "",
        "- Project: `%s`" % PROJECT["name"],
        "- Version: `%s`" % version,
        "- License: `%s`" % PROJECT["license"],
        "",
        "| Component | Version | Upstream source | Packaged as | License | Modified | License text | Redistribution |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for component in sorted(components, key=lambda item: item["name"].lower()):
        lines.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
            markdown(component["name"]),
            markdown(component["version"]),
            markdown(component["source"]),
            markdown(component["packaged_as"]),
            markdown(component["license"]),
            markdown(component["modified"]),
            license_cell(component),
            markdown(component["redistribution"]),
        ))
    lines.extend([
        "",
        "The original license texts are under `licenses/bindings/<component>/`.",
        "`REDISTRIBUTION.md` describes the per-component retention and",
        "source/relink requirements.",
        "",
    ])
    return "\n".join(lines)


def render_redistribution(components):
    lines = [
        "# Redistribution requirements",
        "",
        "This file is generated by `scripts/stage-license-bundle.py`. Preserve the",
        "complete `licenses/` directory when redistributing the pylibseekdb wheel",
        "or the libseekdb SDK. The upstream license and notice texts must remain",
        "verbatim.",
        "",
        "| Component | Files to retain | Redistribution requirement | Source |",
        "| --- | --- | --- | --- |",
    ]
    for component in sorted(components, key=lambda item: item["name"].lower()):
        lines.append("| %s | %s | %s | %s |" % (
            markdown(component["name"]),
            license_cell(component),
            markdown(component["redistribution"]),
            markdown(component["source"]),
        ))
    lines.extend([
        "",
        "## General rules",
        "",
        "- Keep the engine's `licenses/` bundle and the `licenses/bindings/`",
        "  directory together with the package.",
        "- Do not rewrite upstream license or notice text. If a component is",
        "  modified, retain its notice and record the modification in the source",
        "  repository.",
        "- If a component is covered by a copyleft license and is bundled, follow",
        "  its source and relink/replacement requirements.",
        "- Separately installed dependencies are not bundled by this package. A",
        "  downstream application that redistributes them must preserve the license",
        "  materials from the exact resolved versions.",
        "",
    ])
    return "\n".join(lines)


def render_readme(engine_inventory_present):
    lines = [
        "# Third-party license bundle",
        "",
        "This directory has two layers:",
        "",
        "- `licenses/` (this directory): the seekdb engine's license bundle,",
        "  generated by the seekdb source tree. It contains the engine's LICENSE,",
        "  NOTICE, third-party component inventory, redistribution requirements and",
        "  the original license texts of every engine component.",
        "- `licenses/bindings/`: the components added by seekdb-bindings, with the",
        "  project LICENSE, NOTICE, inventory, redistribution requirements and",
        "  license texts.",
        "",
    ]
    if engine_inventory_present:
        lines.extend([
            "Complete inventory: `THIRD_PARTY_COMPONENTS.md` (engine) plus",
            "`bindings/THIRD_PARTY_COMPONENTS.md` (bindings).",
            "",
        ])
    else:
        lines.extend([
            "The engine bundle in this build does not contain a component inventory",
            "(`THIRD_PARTY_COMPONENTS.md`). The engine's inventory is added by the",
            "seekdb source tree; `bindings/THIRD_PARTY_COMPONENTS.md` covers the",
            "bindings layer only.",
            "",
        ])
    return "\n".join(lines)


def copy_file(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(0o644)


def write_manifest(target):
    lines = []
    for path in sorted(target.rglob("*")):
        if path.is_file() and path.name != "MANIFEST.sha256":
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            lines.append("%s  %s" % (digest, path.relative_to(target).as_posix()))
    (target / "MANIFEST.sha256").write_text("\n".join(lines) + "\n", encoding="utf-8")


def stage(sdk_dir, output=None, version=None):
    sdk_dir = Path(sdk_dir)
    output = Path(output) if output else sdk_dir / "licenses"
    if not (sdk_dir / "BUILD-INFO.json").is_file() and not (sdk_dir / "licenses").is_dir():
        raise ValueError("%s does not look like an SDK prefix" % sdk_dir)
    bindings = output / "bindings"
    if bindings.exists():
        shutil.rmtree(bindings)
    bindings.mkdir(parents=True)

    components = []
    notes = []
    for component in load_components():
        resolved, messages = resolve_version(component, sdk_dir)
        component["version"] = resolved
        components.append(component)
        notes.extend(messages)

    for name in PROJECT["license_files"] + PROJECT["notice_files"]:
        copy_file(ROOT / name, bindings / name)
    for component in components:
        directory = bindings / component["directory"]
        directory.mkdir(parents=True, exist_ok=True)
        for relative in component["license_files"]:
            copy_file(LICENSES / component["directory"] / relative, directory / relative)

    version = artifact_version(sdk_dir, version)
    inventory = {
        "schema_version": 1,
        "artifact": {
            "name": PROJECT["name"],
            "version": version,
            "license": PROJECT["license"],
        },
        "components": components,
    }
    (bindings / "THIRD_PARTY_COMPONENTS.json").write_text(
        json.dumps(inventory, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (bindings / "THIRD_PARTY_COMPONENTS.md").write_text(
        render_inventory(components, version), encoding="utf-8")
    (bindings / "REDISTRIBUTION.md").write_text(
        render_redistribution(components), encoding="utf-8")
    write_manifest(bindings)

    readme = output / "README.md"
    if not readme.exists():
        readme.write_text(render_readme(engine_inventory(sdk_dir) is not None), encoding="utf-8")

    print("staged bindings license bundle in %s (%d components, pylibseekdb %s)" % (
        bindings, len(components), version))
    for note in notes:
        print("warning: " + note)
    return bindings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-dir", type=Path, required=True,
                        help="SDK prefix that contains the engine licenses/ directory")
    parser.add_argument("--output", type=Path, default=None,
                        help="output directory (default: <sdk-dir>/licenses)")
    parser.add_argument("--version", default=None)
    arguments = parser.parse_args()
    stage(arguments.sdk_dir, arguments.output, arguments.version)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
