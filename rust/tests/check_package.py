"""Dry-run a crates.io publish, inspect the archive, and build unpacked sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import tomllib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--allow-dirty', action='store_true', help='allow local work in progress; omit for a release')
args = parser.parse_args()
if 'DOCS_RS' in os.environ:
    parser.error('unset DOCS_RS so publication checks verify the real embedded native runtime')
crate = Path(__file__).resolve().parents[1]
metadata = json.loads(subprocess.check_output(['cargo', 'metadata', '--manifest-path', str(crate / 'Cargo.toml'), '--no-deps', '--format-version', '1'], text=True))
package = next(p for p in metadata['packages'] if Path(p['manifest_path']) == crate / 'Cargo.toml')
config = package['metadata']['seekdb']
package_root = f"{package['name']}-{package['version']}"
native_manifest = f"bundles/manifest-{config['native-version']}.json"
assert package['publish'] == ['crates-io'], 'SDK must explicitly target crates.io'
command = ['cargo', 'publish', '--dry-run', '--registry', 'crates-io', '--manifest-path', str(crate / 'Cargo.toml'), '--locked']
if args.allow_dirty:
    command.append('--allow-dirty')
subprocess.run(command, check=True)
# Cargo publish may leave its archive in an internal temporary registry rather
# than package/. Generate the documented cargo-package output explicitly so a
# previous build's .crate can never be mistaken for the one just verified.
command = ['cargo', 'package', '--manifest-path', str(crate / 'Cargo.toml'), '--locked', '--no-verify']
if args.allow_dirty:
    command.append('--allow-dirty')
subprocess.run(command, check=True)
archive = Path(metadata['target_directory']) / 'package' / f'{package_root}.crate'
required = ['LICENSE', 'NOTICE', 'PATENTS', 'THIRD_PARTY_NOTICES.md', 'README.md', 'docs/releasing.md', 'Cargo.toml', 'Cargo.lock', 'build.rs', 'build_cache.rs', 'native_cache.rs', 'bundle_manifest.rs', 'manifest_cache.rs', native_manifest, 'src/lib.rs', 'src/runtime.rs', 'src/native_path.rs', 'examples/first_query.rs', 'examples/async_query.rs']
with tempfile.TemporaryDirectory(prefix='pkg-') as directory:
    with tarfile.open(archive) as source:
        names = set(source.getnames())
        for path in required:
            assert f'{package_root}/{path}' in names, f'package missing {path}'
        assert not any(name.endswith(('.tar.gz', '.so', '.dylib', '.dll', '.exe')) for name in names), 'native build outputs must not enter the source package'
        packaged = tomllib.loads(source.extractfile(f'{package_root}/Cargo.toml').read().decode())
        assert packaged['package']['publish'] == ['crates-io']
        assert packaged['package']['metadata']['seekdb'] == config
        for notice in ['NOTICE', 'PATENTS']:
            assert source.extractfile(f'{package_root}/{notice}').read() == (crate / notice).read_bytes()
        snapshot = source.extractfile(f'{package_root}/{native_manifest}').read()
        assert hashlib.sha256(snapshot).hexdigest() == config['manifest-sha256']
        assert json.loads(snapshot)['package_version'] == config['native-version']
        # This archive was just generated locally by cargo package.
        source.extractall(directory, filter='data')
    manifest = Path(directory) / package_root / 'Cargo.toml'
    env = dict(os.environ, CARGO_TARGET_DIR=str(Path(metadata['target_directory']) / 'package-check'))
    subprocess.run(['cargo', 'check', '--manifest-path', str(manifest), '--locked', '--lib'], env=env, check=True)
print('crates.io publish dry-run and unpacked source-package checks passed; nothing was uploaded.')
