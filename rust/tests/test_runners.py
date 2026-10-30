"""Exercise deployment-runner failure paths with real disposable processes."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    'deployment', Path(__file__).parent / 'consumer' / 'test_deployment.py'
)
deployment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deployment)


class ParallelConsumers(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='runner-')
        self.addCleanup(self.directory.cleanup)
        self.children = []
        self.popen = subprocess.Popen
        self.command = [sys.executable, '-c', 'import time; time.sleep(60)']
        self.addCleanup(self.reap_remaining_children)

    def reap_remaining_children(self):
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait()

    def spawn(self, *args, **kwargs):
        child = self.popen(*args, **kwargs)
        self.children.append(child)
        return child

    def test_timeout_reaps_every_consumer_before_propagating(self):
        with mock.patch.object(deployment.subprocess, 'Popen', side_effect=self.spawn):
            with self.assertRaises(subprocess.TimeoutExpired):
                deployment.run_parallel(self.command, env=os.environ.copy(),
                                        cwd=self.directory.name, timeout=0.1)
        self.assertEqual(len(self.children), 3)
        self.assertTrue(all(child.returncode is not None for child in self.children))

    def test_partial_spawn_failure_reaps_consumers_already_started(self):
        def fail_third_spawn(*args, **kwargs):
            if len(self.children) == 2:
                raise OSError('synthetic spawn failure')
            return self.spawn(*args, **kwargs)

        with mock.patch.object(deployment.subprocess, 'Popen', side_effect=fail_third_spawn):
            with self.assertRaisesRegex(OSError, 'synthetic spawn failure'):
                deployment.run_parallel(self.command, env=os.environ.copy(), cwd=self.directory.name)
        self.assertEqual(len(self.children), 2)
        self.assertTrue(all(child.returncode is not None for child in self.children))

    def test_nonzero_exit_statuses_are_preserved_and_all_children_reaped(self):
        with mock.patch.object(deployment.subprocess, 'Popen', side_effect=self.spawn):
            statuses = deployment.run_parallel([sys.executable, '-c', 'raise SystemExit(7)'],
                                               env=os.environ.copy(), cwd=self.directory.name)
        self.assertEqual(statuses, [7, 7, 7])
        self.assertTrue(all(child.returncode == 7 for child in self.children))


if __name__ == '__main__':
    unittest.main()
