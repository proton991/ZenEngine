"""Check stable executable identity, exclusive ownership and crash recovery."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import rtss_runner


class RtssRunnerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.runner = self.root / 'bin' / '7zFM.exe'
        self.source = self.root / 'release' / 'demo.exe'
        self.source.parent.mkdir()
        self.source.write_bytes(b'release executable')
        self.override = patch.object(rtss_runner, 'RTSS_RUNNER', self.runner)
        self.override.start()
        self.addCleanup(self.override.stop)

    def child(self, body):
        code = ('import os, sys\nfrom pathlib import Path\n'
                f'sys.path.insert(0, {str(Path(rtss_runner.__file__).parent)!r})\n'
                'import rtss_runner\n'
                f'rtss_runner.RTSS_RUNNER = Path({str(self.runner)!r})\n' + body)
        return subprocess.run([sys.executable, '-c', code], capture_output=True, text=True, timeout=10)

    def test_different_builds_reuse_one_path(self):
        baseline = self.root / 'baseline' / 'demo.exe'
        baseline.parent.mkdir()
        baseline.write_bytes(b'baseline executable')
        for source in (self.source, baseline, self.source):
            with rtss_runner.rtss_executable(source) as executable:
                self.assertEqual(executable, self.runner)
                self.assertEqual(executable.read_bytes(), source.read_bytes())
            self.assertTrue(executable.is_file())
        self.assertEqual(len(list(self.root.rglob('7zFM.exe'))), 1)

    def test_dependencies_refresh_with_executable(self):
        library = self.source.with_name('engine.dll')
        for content in (b'first dll', b'updated dll'):
            library.write_bytes(content)
            with rtss_runner.rtss_executable(self.source):
                self.assertEqual((self.runner.parent / library.name).read_bytes(), content)

    def test_concurrent_process_cannot_replace_runner(self):
        with rtss_runner.rtss_executable(self.source):
            result = self.child(f'with rtss_runner.rtss_executable({str(self.source)!r}):\n    pass\n')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('is in use', result.stderr)
            self.assertEqual(self.runner.read_bytes(), self.source.read_bytes())

    def test_crashed_owner_does_not_leave_a_stale_lock(self):
        result = self.child(f'with rtss_runner.rtss_executable({str(self.source)!r}):\n    os._exit(42)\n')
        self.assertEqual(result.returncode, 42)
        with rtss_runner.rtss_executable(self.source) as executable:
            self.assertEqual(executable.read_bytes(), self.source.read_bytes())

    def test_child_failure_releases_lease(self):
        with self.assertRaisesRegex(RuntimeError, 'child failed'):
            with rtss_runner.rtss_executable(self.source):
                raise RuntimeError('child failed')
        with rtss_runner.rtss_executable(self.source) as executable:
            self.assertTrue(executable.is_file())

    def test_copy_failure_releases_lease(self):
        with patch.object(rtss_runner.shutil, 'copy2', side_effect=PermissionError('busy executable')):
            with self.assertRaises(PermissionError):
                with rtss_runner.rtss_executable(self.source):
                    self.fail('Failed staging must not launch a child')
        with rtss_runner.rtss_executable(self.source) as executable:
            self.assertTrue(executable.is_file())

    def test_missing_source_preserves_previous_executable(self):
        with rtss_runner.rtss_executable(self.source):
            pass
        with self.assertRaises(FileNotFoundError):
            with rtss_runner.rtss_executable(self.root / 'missing.exe'):
                self.fail('Missing executable must not launch a child')
        self.assertEqual(self.runner.read_bytes(), self.source.read_bytes())

    def test_sources_already_in_runner_directory(self):
        with rtss_runner.rtss_executable(self.source):
            pass
        with rtss_runner.rtss_executable(self.runner) as executable:
            self.assertEqual(executable, self.runner)
        sibling = self.runner.with_name('demo.exe')
        sibling.write_bytes(b'sibling executable')
        (self.runner.parent / 'engine.dll').write_bytes(b'sibling dll')
        with rtss_runner.rtss_executable(sibling) as executable:
            self.assertEqual(executable.read_bytes(), sibling.read_bytes())


if __name__ == '__main__':
    unittest.main()
