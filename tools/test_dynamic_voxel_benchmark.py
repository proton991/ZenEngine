"""Failure-path checks for the serial benchmark artifact/configuration contract."""
import csv
import copy
import json
from pathlib import Path
import subprocess
import struct
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

import benchmark_dynamic_voxel_gi as benchmark


class BenchmarkTests(unittest.TestCase):
    def test_config_restored_on_success_exception_and_timeout(self):
        for failure in (None, RuntimeError('process failed'), subprocess.TimeoutExpired('engine', 1)):
            with tempfile.TemporaryDirectory() as directory:
                folder = Path(directory)
                config = folder / 'engine.cfg'
                original = b'# exact bytes\r\nkey=value\r\n'
                config.write_bytes(original)
                try:
                    with benchmark.config_lease(config, b'edited', folder):
                        self.assertEqual(config.read_bytes(), b'edited')
                        if failure:
                            raise failure
                except (RuntimeError, subprocess.TimeoutExpired):
                    pass
                self.assertEqual(config.read_bytes(), original)
                self.assertEqual((folder / 'original.cfg').read_bytes(), original)
                self.assertFalse(Path(str(config) + '.m8.lock').exists())

    def test_concurrent_edit_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            config = folder / 'engine.cfg'
            config.write_bytes(b'original')
            with self.assertRaisesRegex(AssertionError, 'preserving'):
                with benchmark.config_lease(config, b'edited', folder):
                    config.write_bytes(b'user edit')
            self.assertEqual(config.read_bytes(), b'user edit')
            self.assertEqual((folder / 'original.cfg').read_bytes(), b'original')

    def test_existing_owner_is_not_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            config = folder / 'engine.cfg'
            config.write_bytes(b'original')
            lock = Path(str(config) + '.m8.lock')
            lock.write_bytes(b'another owner')
            with self.assertRaises(FileExistsError):
                with benchmark.config_lease(config, b'edited', folder):
                    self.fail('Acquired an owned configuration')
            self.assertEqual(config.read_bytes(), b'original')
            self.assertEqual(lock.read_bytes(), b'another owner')

    def test_stale_run_directory_refused_before_process(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(benchmark.subprocess, 'run') as run:
                with self.assertRaises(FileExistsError):
                    benchmark.run_command(['engine'], Path(directory), False, 1)
                run.assert_not_called()

    def test_throughput_requires_complete_fresh_finite_frames(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'frames.csv'
            for rows, valid in (([(0, 2), (1, 4)], True), ([(0, 2)], False),
                                ([(0, 2), (0, 4)], False), ([(1, 2), (2, 4)], False),
                                ([(0, 'nan'), (1, 4)], False), ([(0, 0), (1, 4)], False)):
                with path.open('w', newline='') as target:
                    writer = csv.writer(target)
                    writer.writerow(('frame', 'cpu_frame_ms'))
                    writer.writerows(rows)
                if valid:
                    self.assertEqual(benchmark.throughput_samples(path, 2)['cpu_frame_ms']['median'], 3)
                else:
                    with self.assertRaises(AssertionError):
                        benchmark.throughput_samples(path, 2)

    def test_asset_includes_external_buffers_and_images(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            asset = folder / 'fixture.gltf'
            asset.write_text(json.dumps(dict(buffers=[dict(uri='mesh.bin')], images=[dict(uri='color.png')])) )
            (folder / 'mesh.bin').write_bytes(b'geometry')
            (folder / 'color.png').write_bytes(b'image')
            initial = benchmark.asset_identity(asset)
            self.assertEqual(len(initial['resources']), 2)
            (folder / 'color.png').write_bytes(b'changed')
            self.assertNotEqual(initial, benchmark.asset_identity(asset))

    def test_workgroup_metadata_and_corrupt_instruction_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'gather.spv'
            words = [0x07230203, 0x00010300, 0, 2, 0, (6 << 16) | 16, 1, 17, 128, 1, 1]
            path.write_bytes(struct.pack('<11I', *words))
            self.assertEqual(benchmark.shader_local_size(path), [128, 1, 1])
            words[5] = 16
            path.write_bytes(struct.pack('<11I', *words))
            with self.assertRaisesRegex(AssertionError, 'Truncated'):
                benchmark.shader_local_size(path)


class ProfileIdentityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        folder = Path(temporary.name)
        self.args = SimpleNamespace(executable=folder / 'engine.exe', asset=folder / 'scene.gltf',
            config=folder / 'engine.cfg', shaders=folder / 'shaders', allow_debug=False,
            thread=1, resolution=64, voxelizer='comp', vsync=0, method='cone',
            expect_fallback=False, require_async=False)
        self.args.executable.write_bytes(b'executable')
        self.args.asset.write_text('{"asset":{"version":"2.0"}}')
        self.args.config.write_bytes(b'scene=fixture\r\n')
        self.args.shaders.mkdir()
        (self.args.shaders / 'first.spv').write_bytes(b'first shader')
        (self.args.shaders / 'second.spv').write_bytes(b'second shader')
        self.expected = benchmark.profile_input_identity(self.args)
        self.manifest = dict(schema_version=3, build=dict(ndebug=True), device={},
            settings=dict(rhi_threaded=True, voxel_resolution=64, voxelizer='comp', vsync_requested=False),
            gpu_frame_ms={}, cpu_frame_ms={}, pass_statistics=[],
            inputs=dict(copy.deepcopy(self.expected), algorithm='fnv1a64', shader_enumeration_complete=True))
        self.prefix = folder / 'capture'
        Path(str(self.prefix) + '.frames.csv').write_text('phase,gi_effective_method\nmeasured,cone\n')

    def check_capture(self):
        Path(str(self.prefix) + '.profile.json').write_text(json.dumps(self.manifest))
        # Export statistics have their own regression suite. Exercise the runner's
        # acceptance boundary with independently supplied identity metadata here.
        with mock.patch.object(benchmark.profile, 'validate', return_value=dict(verified=True)):
            return benchmark.check_profile(self.args, self.prefix, expected_inputs=self.expected)

    def test_engine_fingerprint_known_vector(self):
        self.args.executable.write_bytes(b'hello')
        fingerprint = benchmark.profile_file_identity(self.args.executable)
        self.assertEqual(fingerprint['fnv1a64'], 'a430d84680aabd0b')
        self.assertEqual(fingerprint['bytes'], 5)

    def test_matching_capture_accepts_reordered_shader_entries(self):
        self.manifest['inputs']['shaders'].reverse()
        self.assertTrue(self.check_capture()['verification']['verified'])

    def test_wrong_executable_configuration_or_scene_is_rejected(self):
        for name in ('executable', 'configuration', 'scene_document'):
            with self.subTest(name=name):
                saved = copy.deepcopy(self.manifest['inputs'][name])
                self.manifest['inputs'][name]['path'] += '.different'
                with self.assertRaisesRegex(AssertionError, 'Profile input differs'):
                    self.check_capture()
                self.manifest['inputs'][name] = saved

    def test_changed_content_or_size_is_rejected(self):
        for name in ('executable', 'configuration', 'scene_document', 'shaders'):
            for field, value in (('fnv1a64', '0000000000000000'), ('bytes', 999)):
                with self.subTest(name=name, field=field):
                    saved = copy.deepcopy(self.manifest['inputs'])
                    entry = self.manifest['inputs'][name]
                    if name == 'shaders':
                        entry = entry[0]
                    entry[field] = value
                    with self.assertRaises(AssertionError):
                        self.check_capture()
                    self.manifest['inputs'] = saved

    def test_wrong_shader_directory_is_rejected_even_for_identical_bytes(self):
        self.manifest['inputs']['shaders'][0]['path'] += '.different'
        with self.assertRaisesRegex(AssertionError, 'Profile shaders differ'):
            self.check_capture()

    def test_missing_extra_duplicate_or_empty_shader_set_is_rejected(self):
        shaders = self.manifest['inputs']['shaders']
        extra = dict(shaders[0], path=str(self.args.shaders / 'extra.spv'))
        for entries in (shaders[:1], shaders + [extra], shaders + shaders[:1], []):
            with self.subTest(entries=entries):
                self.manifest['inputs']['shaders'] = entries
                with self.assertRaises(AssertionError):
                    self.check_capture()

    def test_unreadable_or_incomplete_inputs_are_rejected(self):
        for name in ('executable', 'configuration', 'scene_document', 'shaders'):
            with self.subTest(name=name):
                saved = copy.deepcopy(self.manifest['inputs'])
                entry = self.manifest['inputs'][name]
                if name == 'shaders':
                    entry = entry[0]
                entry['readable'] = False
                with self.assertRaisesRegex(AssertionError, 'Unreadable'):
                    self.check_capture()
                self.manifest['inputs'] = saved
        self.manifest['inputs']['shader_enumeration_complete'] = False
        with self.assertRaisesRegex(AssertionError, 'Incomplete shader enumeration'):
            self.check_capture()

    def test_capture_compares_against_frozen_inputs(self):
        self.args.asset.write_text('{"asset":{"version":"2.0"},"scenes":[{}]}')
        self.manifest['inputs']['scene_document'] = benchmark.profile_file_identity(self.args.asset)
        with self.assertRaisesRegex(AssertionError, 'Profile input differs'):
            self.check_capture()


if __name__ == '__main__':
    unittest.main()
