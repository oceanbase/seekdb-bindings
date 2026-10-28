"""Check the actual Cargo source archive, then compile its unpacked contents."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

crate = Path(__file__).resolve().parents[1]
metadata = json.loads(subprocess.check_output(['cargo', 'metadata', '--manifest-path', str(crate / 'Cargo.toml'), '--no-deps', '--format-version', '1'], text=True))
package = next(p for p in metadata['packages'] if Path(p['manifest_path']) == crate / 'Cargo.toml')
config = package['metadata']['seekdb']
package_root = f"{package['name']}-{package['version']}"
native_manifest = f"bundles/manifest-{config['native-version']}.json"
subprocess.run(['cargo', 'package', '--manifest-path', str(crate / 'Cargo.toml'), '--locked', '--allow-dirty', '--no-verify'], check=True)
archive = Path(metadata['target_directory']) / 'package' / f'{package_root}.crate'
required = ['LICENSE', 'Cargo.toml', 'Cargo.lock', 'build.rs', 'build_cache.rs', 'native_cache.rs', 'bundle_manifest.rs', 'manifest_cache.rs', native_manifest, 'src/lib.rs', 'src/runtime.rs', 'src/native_path.rs']
with tempfile.TemporaryDirectory(prefix='pkg-') as directory:
    with tarfile.open(archive) as source:
        names = set(source.getnames())
        for path in required:
            assert f'{package_root}/{path}' in names, f'package missing {path}'
        snapshot = source.extractfile(f'{package_root}/{native_manifest}').read()
        assert hashlib.sha256(snapshot).hexdigest() == config['manifest-sha256']
        assert json.loads(snapshot)['package_version'] == config['native-version']
        # This archive was just generated locally by cargo package.
        source.extractall(directory, filter='data')
    manifest = Path(directory) / package_root / 'Cargo.toml'
    env = dict(os.environ, CARGO_TARGET_DIR=str(Path(metadata['target_directory']) / 'package-check'))
    subprocess.run(['cargo', 'check', '--manifest-path', str(manifest), '--locked', '--lib'], env=env, check=True)
print('Source package contains and builds all required native bundle inputs; publish remains disabled.')
