"""Build docs without a native cache, then verify normal builds restore embedding."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

crate = Path(__file__).resolve().parents[1]
manifest = str(crate / 'Cargo.toml')
metadata = json.loads(subprocess.check_output([
    'cargo', 'metadata', '--manifest-path', manifest, '--no-deps', '--format-version', '1'
], text=True))
target = Path(metadata['target_directory']) / 'docs-check'


def run(arguments, env):
    result = subprocess.run(['cargo', *arguments, '--manifest-path', manifest, '--locked', '--offline'],
                            env=env, text=True, capture_output=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)


def generated():
    paths = list(target.glob('debug/build/seekdb-*/out/native_bundle.rs'))
    assert paths, 'build script did not generate native metadata'
    return max(paths, key=lambda path: path.stat().st_mtime_ns)


with tempfile.TemporaryDirectory(prefix='seekdb-docs-') as directory:
    root = Path(directory)
    cache = root / 'unused-cache'
    normal_env = dict(os.environ, CARGO_TARGET_DIR=str(target))
    normal_env.pop('DOCS_RS', None)
    docs_env = dict(normal_env, DOCS_RS='1', SEEKDB_CACHE_DIR=str(cache),
                    SEEKDB_DOWNLOAD_BASE_URL='http://invalid.example/docs-must-not-download',
                    HTTP_PROXY='http://127.0.0.1:9', HTTPS_PROXY='http://127.0.0.1:9',
                    ALL_PROXY='http://127.0.0.1:9', NO_PROXY='', RUSTDOCFLAGS='-D warnings')
    run(['doc', '--no-deps', '--lib'], docs_env)
    assert (target / 'doc' / 'seekdb' / 'index.html').is_file()
    assert 'pub const ARCHIVE: &[u8] = &[];' in generated().read_text()
    assert not cache.exists(), 'docs build touched the runtime cache'
    # Accidental executable builds with DOCS_RS must report a clear error
    # before attempting extraction or loading a placeholder native payload.
    result = subprocess.run([
        'cargo', 'run', '--manifest-path', manifest, '--locked', '--offline',
        '--example', 'first_query', '--', str(root / 'database')
    ], env=docs_env, text=True, capture_output=True)
    assert result.returncode != 0, result.stdout
    assert 'rebuild without DOCS_RS' in result.stderr, result.stderr
    assert not cache.exists()
    run(['check', '--lib'], normal_env)
    assert 'include_bytes!' in generated().read_text(), 'DOCS_RS change did not restore native embedding'
print('Offline docs, documentation-only runtime guard, and normal-build embedding passed.')
