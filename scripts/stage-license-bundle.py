#!/usr/bin/env python3
"""Stage the bindings layer of the third-party license bundle.

The SDK prefix produced by ``.github/scripts/package.py`` already carries the
engine's license bundle under ``licenses/``.  This script validates the
bindings component corpus in ``licenses/`` and renders the bindings layer next
to it: the project LICENSE and NOTICE, the component inventory, the
redistribution requirements, a checksum manifest, and the original license
texts.  Without ``--sdk-dir`` it renders the bindings layer alone, which is
how a wheel built without the SDK gets its license materials.  ``--verify``
checks a rendered bindings directory against its own manifest and inventory.
"""

import argparse
import hashlib
import json
import os
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


def build_requirement_version(name):
    """Return the exact ``name==x.y.z`` pin in python/pyproject.toml, if any."""
    path = ROOT / "python" / "pyproject.toml"
    if not path.is_file():
        return None
    section = re.search(r"^\[build-system\](.*?)(?=^\[|\Z)", path.read_text(encoding="utf-8"),
                        re.MULTILINE | re.DOTALL)
    if not section:
        return None
    match = re.search(r'"%s==([^"\s]+)"' % re.escape(name), section.group(1))
    return match.group(1) if match else None


def sdk_header_version(spec):
    """Return the version recorded in a platform SDK header, if the SDK is present."""
    root = os.environ.get(spec.get("env", ""))
    if not root:
        return None
    path = Path(root) / spec["header"]
    if not path.is_file():
        return None
    match = re.search(spec["pattern"], path.read_text(encoding="utf-8", errors="replace"))
    return spec["format"].format(*match.groups()) if match else None


def engine_inventory(sdk_dir):
    if sdk_dir is None:
        return None
    path = Path(sdk_dir) / "licenses" / "THIRD_PARTY_COMPONENTS.json"
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def engine_component_version(sdk_dir, name):
    inventory = engine_inventory(sdk_dir)
    if not inventory or not name:
        return None
    for component in inventory.get("components", []):
        if component.get("name", "").lower() == name.lower():
            return component.get("version")
    return None


def system_library(component, layout):
    """Return the system-library descriptor when the layout provides the component itself."""
    spec = component.get("system_library")
    if spec and layout and layout in spec.get("layouts", ()):
        return spec
    return None


def resolve_version(component, sdk_dir, layout=None):
    """Return (version, notes) after applying version_from and version_check."""
    version = component["version"]
    notes = []
    system = system_library(component, layout)
    sources = component.get("version_from", [])
    if isinstance(sources, dict):
        sources = [sources]
    for source in sources:
        if "build_requirement" in source:
            resolved = build_requirement_version(source["build_requirement"])
            if resolved:
                version = resolved
                break
            notes.append("%s has no exact %s== build requirement; using %s" % (
                component["name"], source["build_requirement"], version))
        elif "python_package" in source:
            resolved = python_package_version(source["python_package"])
            if resolved:
                version = resolved
                break
            notes.append("%s version not resolvable at build time; using %s" % (
                component["name"], version))
    if system:
        # The platform provides this library, so the dependency source that the
        # vendored description and version_check refer to is not compiled.
        resolved = sdk_header_version(system.get("version_from", {}))
        if resolved:
            version = resolved
        notes.append("%s is provided by %s and is not bundled; version recorded as %s" % (
            component["name"], layout, version or "unspecified"))
        return version, notes
    check = component.get("version_check")
    if check:
        if "engine_component" in check:
            actual = engine_component_version(sdk_dir, check["engine_component"])
            if actual is None:
                notes.append("%s; %s version not cross-checked" % (
                    "no engine inventory is available" if engine_inventory(sdk_dir) is None
                    else "engine inventory has no %s entry" % check["engine_component"], component["name"]))
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
    if sdk_dir is None:
        raise ValueError("--version is required when --sdk-dir is not given")
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


def render_readme(engine_inventory_present, engine_layer=True):
    if engine_layer:
        layers = [
            "- `licenses/` (this directory): the seekdb engine's license bundle,",
            "  generated by the seekdb source tree. It contains the engine's LICENSE,",
            "  NOTICE, third-party component inventory, redistribution requirements and",
            "  the original license texts of every engine component.",
        ]
    else:
        layers = [
            "- `licenses/` (this directory): the package was built without the seekdb",
            "  SDK, so the engine's license bundle is not here; a package built from the",
            "  SDK carries it in this directory.",
        ]
    lines = [
        "# Third-party license bundle",
        "",
        "This directory has two layers:",
        "",
        *layers,
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
    elif engine_layer:
        lines.extend([
            "The engine bundle in this build does not contain a component inventory",
            "(`THIRD_PARTY_COMPONENTS.md`). The engine's inventory is added by the",
            "seekdb source tree; `bindings/THIRD_PARTY_COMPONENTS.md` covers the",
            "bindings layer only.",
            "",
        ])
    else:
        lines.extend([
            "`bindings/THIRD_PARTY_COMPONENTS.md` covers the bindings layer; the",
            "engine's components are listed in the inventory of the SDK the engine",
            "was packaged with.",
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


def stage(sdk_dir, output=None, version=None, layout=None, tls_provider=None):
    if sdk_dir is not None:
        sdk_dir = Path(sdk_dir)
        if not (sdk_dir / "BUILD-INFO.json").is_file() and not (sdk_dir / "licenses").is_dir():
            raise ValueError("%s does not look like an SDK prefix" % sdk_dir)
    elif not output:
        raise ValueError("--output is required when --sdk-dir is not given")
    output = Path(output) if output else sdk_dir / "licenses"
    bindings = output / "bindings"
    if bindings.exists():
        shutil.rmtree(bindings)
    bindings.mkdir(parents=True)

    components = []
    notes = []
    for component in load_components():
        providers = component.get("tls_providers")
        if providers and tls_provider and tls_provider not in providers:
            notes.append("%s is not linked with tls_provider=%s; omitted from the inventory" % (
                component["name"], tls_provider))
            continue
        system = system_library(component, layout)
        if system:
            component["packaged_as"] = system["packaged_as"]
            component["modified"] = "Platform system library; not built or modified by this project"
        resolved, messages = resolve_version(component, sdk_dir, layout)
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
        engine_layer = ((output / "LICENSE").is_file() or (output / "THIRD_PARTY_COMPONENTS.json").is_file()
                        or engine_inventory(sdk_dir) is not None)
        readme.write_text(render_readme(engine_inventory(sdk_dir) is not None, engine_layer),
                          encoding="utf-8")

    print("staged bindings license bundle in %s (%d components, pylibseekdb %s)" % (
        bindings, len(components), version))
    for note in notes:
        print("warning: " + note)
    return bindings


def verify_bundle(bindings):
    """Return the problems found in a rendered bindings directory.

    Every inventory license file and every required file must be listed in a
    non-empty manifest with a matching digest, nothing else may be present, and
    a version derived from an exact build requirement must still match the pin.
    """
    bindings = Path(bindings)
    required = ("LICENSE", "NOTICE", "THIRD_PARTY_COMPONENTS.json", "REDISTRIBUTION.md", "MANIFEST.sha256")
    missing = [name for name in required if not (bindings / name).is_file()]
    if missing:
        return ["missing bindings license files: " + ", ".join(missing)]
    problems = []
    entries = {}
    for line in (bindings / "MANIFEST.sha256").read_text(encoding="utf-8").splitlines():
        checksum, separator, relative = line.partition("  ")
        if not separator or not relative.strip():
            problems.append("malformed manifest line: %r" % line)
            continue
        entries[relative] = checksum
    if not entries:
        problems.append("bindings license manifest is empty")
    inventory = json.loads((bindings / "THIRD_PARTY_COMPONENTS.json").read_text(encoding="utf-8"))
    expected = set(required) - {"MANIFEST.sha256"}
    for component in inventory.get("components", []):
        for relative in component.get("license_files", []):
            expected.add("%s/%s" % (component["directory"], relative))
        sources = component.get("version_from", [])
        for source in (sources if isinstance(sources, list) else [sources]):
            if isinstance(source, dict) and "build_requirement" in source:
                pinned = build_requirement_version(source["build_requirement"])
                if pinned and pinned != component.get("version"):
                    problems.append("%s version %s differs from the %s==%s build requirement" % (
                        component["name"], component.get("version"), source["build_requirement"], pinned))
    for relative in sorted(expected):
        if not (bindings / relative).is_file():
            problems.append("missing " + relative)
        elif relative not in entries:
            problems.append("not listed in the manifest: " + relative)
    present = {path.relative_to(bindings).as_posix() for path in bindings.rglob("*")
               if path.is_file()} - {"MANIFEST.sha256"}
    for relative in sorted(present - set(entries)):
        problems.append("not listed in the manifest: " + relative)
    for relative in sorted(present & set(entries)):
        if hashlib.sha256((bindings / relative).read_bytes()).hexdigest() != entries[relative]:
            problems.append("manifest digest mismatch: " + relative)
    return sorted(set(problems))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-dir", type=Path, default=None,
                        help="SDK prefix that contains the engine licenses/ directory")
    parser.add_argument("--output", type=Path, default=None,
                        help="output directory (default: <sdk-dir>/licenses)")
    parser.add_argument("--version", default=None,
                        help="pylibseekdb version recorded in the inventory (required without --sdk-dir)")
    parser.add_argument("--layout", default=None,
                        help="packaging layout (linux, macos, windows, android); selects system-library records")
    parser.add_argument("--tls-provider", default=None,
                        help="TLS provider of the build (openssl or schannel); omits the records it excludes")
    parser.add_argument("--verify", type=Path, default=None, metavar="DIR",
                        help="check a rendered bindings/ directory and exit non-zero on problems")
    arguments = parser.parse_args()
    if arguments.verify:
        problems = verify_bundle(arguments.verify)
        for problem in problems:
            print("error: " + problem)
        if problems:
            raise SystemExit(1)
        print("bindings license bundle OK: %s" % arguments.verify)
        return
    stage(arguments.sdk_dir, arguments.output, arguments.version,
          arguments.layout, arguments.tls_provider)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
