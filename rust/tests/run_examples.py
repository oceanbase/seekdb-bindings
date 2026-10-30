"""Run the synchronous and asynchronous SQL examples with disposable data."""
from pathlib import Path
import subprocess
import tempfile

manifest = Path(__file__).resolve().parents[1] / 'Cargo.toml'
for example in ['first_query', 'async_query']:
    with tempfile.TemporaryDirectory(prefix='sq-', ignore_cleanup_errors=True) as directory:
        subprocess.run(['cargo', 'run', '--manifest-path', str(manifest), '--locked', '--example', example, '--', str(Path(directory) / 'database')], check=True)
