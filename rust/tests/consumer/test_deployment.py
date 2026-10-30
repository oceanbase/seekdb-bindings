#!/usr/bin/env python3
"""Exercise installed applications and Rust cdylibs with the build tree hidden."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time

CONSUMER = Path(__file__).resolve().parent


def run(command, **kwargs):
    print('+', *map(str, command), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def remove_tree(path):
    # Windows can keep the child server's executable mapped briefly after the
    # lifecycle client exits. Retry cleanup; never unlink a live DLL in place.
    for attempt in range(60):
        try:
            shutil.rmtree(path)
            return
        except FileNotFoundError:
            return
        except PermissionError:
            if attempt == 59:
                raise
            time.sleep(1)


def run_parallel(command, *, env, cwd, count=3, timeout=180):
    children = []
    try:
        for _ in range(count):
            children.append(subprocess.Popen(command, env=env, cwd=cwd))
        return [child.wait(timeout=timeout) for child in children]
    finally:
        # Reap every child before filesystem cleanup, including when waiting
        # times out or a later spawn fails after earlier consumers started.
        for child in children:
            if child.poll() is None:
                child.kill()
            child.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-noexec', action='store_true')
    args = parser.parse_args()
    system = platform.system()
    if args.check_noexec and system != 'Linux':
        parser.error('--check-noexec requires Linux and sudo mount permission')
    root = Path(tempfile.mkdtemp(prefix='sdk-')).resolve()
    noexec = None
    try:
        env = os.environ.copy()
        target = root / 'target'
        env['CARGO_TARGET_DIR'] = str(target)
        run(['cargo', 'install', '--path', CONSUMER / 'app', '--root', root / 'installed', '--debug', '--locked'], env=env)
        run(['cargo', 'build', '--manifest-path', CONSUMER / 'Cargo.toml', '--package', 'seekdb-consumer-ffi', '--locked'], env=env)
        deployed = root / 'deployed'
        deployed.mkdir()
        library_name = {'Linux': 'libseekdb_consumer_ffi.so', 'Darwin': 'libseekdb_consumer_ffi.dylib', 'Windows': 'seekdb_consumer_ffi.dll'}[system]
        shutil.copy2(target / 'debug' / library_name, deployed)
        installed = root / 'installed' / 'bin' / ('seekdb-consumer-app.exe' if system == 'Windows' else 'seekdb-consumer-app')
        copied = deployed / installed.name
        shutil.copy2(installed, copied)
        target.rename(root / 'hidden-target')
        for name in ['LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH']:
            env.pop(name, None)
        # A runtime attempt to download would fail, while embedded extraction
        # continues to work. The application must need only its own artifact.
        env.update(SEEKDB_DOWNLOAD_BASE_URL='https://127.0.0.1:9', HTTP_PROXY='http://127.0.0.1:9', HTTPS_PROXY='http://127.0.0.1:9', ALL_PROXY='http://127.0.0.1:9', NO_PROXY='')
        cache = deployed / 'cache'
        env['SEEKDB_CACHE_DIR'] = str(cache)
        run([installed, deployed / 'db-install'], env=env, cwd=deployed)
        markers = list(cache.rglob('.complete'))
        assert len(markers) == 1
        before = markers[0].stat().st_mtime_ns
        run([copied, deployed / 'db-copy'], env=env, cwd=deployed)
        assert markers[0].stat().st_mtime_ns == before, 'complete cache was re-extracted'
        # Repair a missing native file between consumers, including Windows.
        marker = json.loads(markers[0].read_text())
        native = markers[0].parent / marker['native_dir']
        victim = native / ('seekdb.lib' if system == 'Windows' else 'seekdb')
        victim.unlink()
        run([copied, deployed / 'db-repair'], env=env, cwd=deployed)
        remove_tree(cache)
        run([installed, deployed / 'db-cleanup'], env=env, cwd=deployed)
        # Independent real consumers race on a shared cold cache and on repair.
        # Same data directory avoids allocating several 1 GiB server instances.
        for repair in [False, True]:
            if repair:
                marker_path = next(cache.rglob('.complete'))
                marker = json.loads(marker_path.read_text())
                native = marker_path.parent / marker['native_dir']
                (native / ('seekdb.lib' if system == 'Windows' else 'seekdb')).unlink()
            else:
                remove_tree(cache)
            statuses = run_parallel([str(copied), str(deployed / 'db-race')], env=env, cwd=deployed)
            assert statuses == [0, 0, 0], statuses
        ffi_env = dict(env, SEEKDB_CACHE_DIR=str(deployed / 'ffi-cache'))
        run([sys.executable, CONSUMER / 'probe_dynamic_library.py', deployed / library_name, deployed / 'ffi-db'], env=ffi_env, cwd=deployed)
        remove_tree(deployed / 'ffi-cache')
        run([sys.executable, CONSUMER / 'probe_dynamic_library.py', deployed / library_name, deployed / 'ffi-db'], env=ffi_env, cwd=deployed)
        if args.check_noexec:
            noexec = root / 'noexec'
            noexec.mkdir()
            run(['sudo', 'mount', '-t', 'tmpfs', '-o', 'noexec,nosuid,nodev,size=1m', 'tmpfs', noexec])
            run(['sudo', 'chown', f'{os.getuid()}:{os.getgid()}', noexec])
            noexec.chmod(0o700)
            result = subprocess.run([str(copied), str(deployed / 'noexec-db')], env=dict(env, SEEKDB_CACHE_DIR=str(noexec)), text=True, capture_output=True)
            assert result.returncode != 0
            assert 'is on a filesystem mounted with noexec' in result.stderr, result.stderr
            assert 'SEEKDB_CACHE_DIR' in result.stderr
        print('Deployment checks passed:', system, platform.machine(), flush=True)
    finally:
        if noexec is not None and os.path.ismount(noexec):
            run(['sudo', 'umount', noexec])
        remove_tree(root)


if __name__ == '__main__':
    main()
