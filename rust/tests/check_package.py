"""Check the actual Cargo source archive, then compile its unpacked contents."""
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

crate = Path(__file__).resolve().parents[1]
metadata = json.loads(subprocess.check_output(['cargo', 'metadata', '--manifest-path', str(crate / 'Cargo.toml'), '--no-deps', '--format-version', '1'], text=True))
subprocess.run(['cargo', 'package', '--manifest-path', str(crate / 'Cargo.toml'), '--locked', '--allow-dirty', '--no-verify'], check=True)
archive = Path(metadata['target_directory']) / 'package' / 'seekdb-1.4.0.crate'
required = ['LICENSE', 'Cargo.toml', 'Cargo.lock', 'build.rs', 'build_cache.rs', 'native_cache.rs', 'bundle_manifest.rs', 'bundles/manifest-1.4.0.0-r2.json', 'src/lib.rs', 'src/runtime.rs', 'src/native_path.rs']
with tempfile.TemporaryDirectory(prefix='pkg-') as directory:
    with tarfile.open(archive) as source:
        names = set(source.getnames())
        for path in required:
            assert 'seekdb-1.4.0/' + path in names, f'package missing {path}'
        # This archive was just generated locally by cargo package.
        source.extractall(directory, filter='data')
    manifest = Path(directory) / 'seekdb-1.4.0' / 'Cargo.toml'
    env = dict(os.environ, CARGO_TARGET_DIR=str(Path(metadata['target_directory']) / 'package-check'))
    subprocess.run(['cargo', 'check', '--manifest-path', str(manifest), '--locked', '--lib'], env=env, check=True)
print('Source package contains and builds all required native bundle inputs; publish remains disabled.')
