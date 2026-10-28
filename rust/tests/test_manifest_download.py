"""Exercise real Cargo builds with a remote manifest, offline fallback and reuse.

Run after cargo check with SEEKDB_CACHE_DIR set, so the official runtime archive
can seed an isolated native cache without downloading another large bundle.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

CRATE = Path(__file__).resolve().parents[1]
DEFAULT_BASE = 'https://obbusiness-private.oss-cn-shanghai.aliyuncs.com/download-center/opensource/libseekdb/'


def main():
    metadata = json.loads(subprocess.check_output([
        'cargo', 'metadata', '--manifest-path', str(CRATE / 'Cargo.toml'), '--no-deps', '--format-version', '1'
    ], text=True))
    package = next(p for p in metadata['packages'] if Path(p['manifest_path']) == CRATE / 'Cargo.toml')
    config = package['metadata']['seekdb']
    version = config['native-version']
    host = next(line.removeprefix('host: ') for line in subprocess.check_output(['rustc', '-vV'], text=True).splitlines() if line.startswith('host: '))
    target = os.environ.get('CARGO_BUILD_TARGET', host)
    source_root = os.environ.get('SEEKDB_CACHE_DIR')
    if not source_root:
        raise SystemExit('Set SEEKDB_CACHE_DIR and run cargo check before this integration test.')
    seeds = list((Path(source_root) / target / version).glob('*/bundle.tar.gz'))
    if not seeds:
        raise SystemExit(f'No prepared {target}/{version} runtime archive in SEEKDB_CACHE_DIR')
    subprocess.run(['cargo', 'package', '--manifest-path', str(CRATE / 'Cargo.toml'), '--locked', '--allow-dirty', '--no-verify'], check=True)
    package_root = f"{package['name']}-{package['version']}"
    target_dir = Path(metadata['target_directory'])
    archive = target_dir / 'package' / f'{package_root}.crate'
    manifest_name = f'manifest-{version}.json'
    with tempfile.TemporaryDirectory(prefix='md-') as directory:
        root = Path(directory).resolve()
        cache = root / 'cache'
        for seed in seeds:
            destination = cache / target / version / seed.parent.name / seed.name
            destination.parent.mkdir(parents=True)
            shutil.copyfile(seed, destination)
        shared_manifest = cache / 'manifests' / version / config['manifest-sha256'] / 'manifest.json'
        build_target = root / 'target'
        env = dict(os.environ, SEEKDB_CACHE_DIR=str(cache), CARGO_TARGET_DIR=str(build_target))
        base = os.environ.get('SEEKDB_DOWNLOAD_BASE_URL', DEFAULT_BASE).rstrip('/')

        def build(name, base_url, keep_snapshot):
            source = root / name
            source.mkdir()
            with tarfile.open(archive) as package_archive:
                package_archive.extractall(source, filter='data')
            crate = source / package_root
            if not keep_snapshot:
                (crate / 'bundles' / manifest_name).unlink()
            # Cargo can reuse OUT_DIR across relocated copies of the same
            # package. Remove only this test's build-local manifest snapshots
            # so each scenario exercises the intended shared/network source.
            for local in build_target.glob(f'debug/build/seekdb-*/out/{manifest_name}'):
                local.unlink()
            result = subprocess.run([
                'cargo', 'check', '--manifest-path', str(crate / 'Cargo.toml'), '--locked', '--lib'
            ], env=dict(env, SEEKDB_DOWNLOAD_BASE_URL=base_url), text=True, capture_output=True)
            if result.returncode:
                raise AssertionError(f'{name} build failed:\n{result.stdout}\n{result.stderr}')
            print(f'Manifest build scenario passed: {name}', flush=True)
            assert hashlib.sha256(shared_manifest.read_bytes()).hexdigest() == config['manifest-sha256']
            return result.stdout + result.stderr

        # A cold request is mandatory: no metadata cache and no packaged copy.
        # Omitting the trailing slash also checks actual URL normalization.
        build('remote-mirror-prefix', base, keep_snapshot=False)
        shutil.rmtree(cache / 'manifests')
        log = build('offline-packaged-fallback', 'https://127.0.0.1:9', keep_snapshot=True)
        assert 'using the verified packaged native manifest' in log, log
        # No packaged copy is available here; a request could not succeed.
        log = build('offline-shared-cache', 'https://127.0.0.1:9', keep_snapshot=False)
        assert 'using the verified packaged native manifest' not in log, log
    print('Remote manifest, packaged fallback and shared-cache reuse passed.')


if __name__ == '__main__':
    main()
