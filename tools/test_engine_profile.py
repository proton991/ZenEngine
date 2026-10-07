"""CPU regression checks for the profile acceptance reader and summary contract."""
import csv
import json
from pathlib import Path
import tempfile
import unittest

import validate_engine_profile as profile


class ProfileTests(unittest.TestCase):
    def write_rows(self, prefix, suffix, rows):
        with Path(str(prefix) + suffix).open('w', encoding='utf-8', newline='') as target:
            writer = csv.DictWriter(target, rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)

    def write_manifest(self, prefix, manifest):
        Path(str(prefix) + '.profile.json').write_text(json.dumps(manifest), encoding='utf-8')

    def test_statistics_empty_single_even_and_tail(self):
        self.assertIsNone(profile.statistics_for([])['p95'])
        self.assertEqual(profile.statistics_for([7])['median'], 7)
        self.assertEqual(profile.statistics_for([4, 1, 3, 2])['median'], 2.5)
        self.assertEqual(profile.statistics_for(range(1, 101))['p95'], 95)

    def test_nonfinite_and_negative_samples_are_rejected(self):
        for invalid in ('nan', 'inf', '-1'):
            with self.assertRaises(AssertionError):
                profile.finite_nonnegative(invalid)

    def write_capture(self, folder):
        prefix = folder / 'profile, escaped'
        label = 'pass,"quoted"\nnext line'
        frame = dict(run_id='capture1', frame_index='12', phase='cold', phase_frame='0',
                     cpu_frame_ms='2', succeeded='true')
        row = dict(run_id='capture1', graph_record='0', frame_index='12', phase='cold',
                   graph='graph,"name"', execution='1', node_id='0', **{'pass': label}, queue='graphics',
                   queue_equivalence='0', cpu_record_us='3', gpu_status='unsupported', gpu_us='')
        for suffix, record in (('.frames.csv', frame), ('.passes.csv', row)):
            with Path(str(prefix) + suffix).open('w', encoding='utf-8', newline='') as target:
                writer = csv.DictWriter(target, record.keys())
                writer.writeheader()
                writer.writerow(record)
        manifest = dict(schema_version=1, run_id='capture1', run_succeeded=True,
                        input_files_unchanged=True, instrumented=True, frame_count=1, pass_count=1,
                        dropped=dict(frames=0, graphs=0, passes=0),
                        inputs=dict(configuration=dict(readable=False)),
                        cpu_frame_ms={phase: profile.statistics_for([2] if phase == 'cold' else [])
                                      for phase in ('cold', 'warmup', 'measured')},
                        graphs=[dict(record=0, frame_index=12, graph=row['graph'], execution=1, phase='cold',
                                     node_count=1, omitted_nodes=0, omitted_submissions=0)],
                        pass_statistics=[dict(phase='cold', graph=row['graph'], **{'pass': label},
                                              queue='graphics', queue_equivalence=0, gpu_unavailable=1,
                                              cpu_record_us=profile.statistics_for([3]),
                                              gpu_us=profile.statistics_for([]))])
        Path(str(prefix) + '.profile.json').write_text(json.dumps(manifest), encoding='utf-8')
        return prefix, row, manifest

    def test_escaped_fields_and_unavailable_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, _, _ = self.write_capture(Path(directory))
            self.assertTrue(profile.validate(prefix)['verified'])
            with self.assertRaises(AssertionError):
                profile.validate(prefix, require_gpu=True)

    def test_fake_zero_and_mixed_run_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, row, _ = self.write_capture(Path(directory))
            for field, value in (('gpu_us', '0'), ('run_id', 'old_capture')):
                changed = dict(row, **{field: value})
                with Path(str(prefix) + '.passes.csv').open('w', encoding='utf-8', newline='') as target:
                    writer = csv.DictWriter(target, changed.keys())
                    writer.writeheader()
                    writer.writerow(changed)
                with self.assertRaises(AssertionError):
                    profile.validate(prefix)

    def test_stale_summary_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, _, manifest = self.write_capture(Path(directory))
            manifest['pass_statistics'][0]['cpu_record_us']['median'] = 100
            Path(str(prefix) + '.profile.json').write_text(json.dumps(manifest), encoding='utf-8')
            with self.assertRaises(AssertionError):
                profile.validate(prefix)

    def test_truncated_or_missing_passes_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, _, manifest = self.write_capture(Path(directory))
            for field, value in (('omitted_nodes', 1), ('omitted_submissions', 1), ('node_count', 2)):
                original = manifest['graphs'][0][field]
                manifest['graphs'][0][field] = value
                Path(str(prefix) + '.profile.json').write_text(json.dumps(manifest), encoding='utf-8')
                with self.assertRaises(AssertionError):
                    profile.validate(prefix)
                manifest['graphs'][0][field] = original

    def write_frame_capture(self, folder, statuses=('available',)):
        prefix, _, manifest = self.write_capture(folder)
        frames = []
        for position, status in enumerate(statuses):
            frames.append(dict(run_id='capture1', frame_index=str(12 + position),
                               phase='cold' if position == 0 else 'measured',
                               phase_frame=str(position), cpu_frame_ms='2', succeeded='true',
                               gpu_status=status, gpu_frame_ms='1.5' if status == 'available' else '',
                               gpu_intervals='2', gpu_excluded_intervals='1'))
        manifest.update(schema_version=2, requested_frames=len(frames), requested_warmup_frames=0,
                        gi_start_frame=0, gpu_frame_scope='elapsed native command-buffer span',
                        frame_count=len(frames))
        manifest['dropped']['gpu_frames'] = 0
        manifest['cpu_frame_ms'] = {
            phase: profile.statistics_for([2 for frame in frames if frame['phase'] == phase])
            for phase in profile.PHASES}
        manifest['gpu_frame_ms'] = {
            phase: profile.statistics_for([1.5 for frame in frames
                                           if frame['phase'] == phase and frame['gpu_status'] == 'available'])
            for phase in profile.PHASES}
        manifest['gpu_frame_unavailable'] = {
            phase: sum(frame['phase'] == phase and frame['gpu_status'] != 'available' for frame in frames)
            for phase in profile.PHASES}
        self.write_rows(prefix, '.frames.csv', frames)
        self.write_manifest(prefix, manifest)
        return prefix, frames, manifest

    def test_frame_measurements_and_independent_requirements(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, _, _ = self.write_frame_capture(Path(directory))
            result = profile.validate(prefix, require_frame_gpu=True)
            self.assertTrue(result['verified'])
            self.assertEqual(result['gpu_frame_statuses'], {'available': 1})
            with self.assertRaises(AssertionError):
                profile.validate(prefix, require_gpu=True, require_frame_gpu=True)
            with Path(str(prefix) + '.passes.csv').open(encoding='utf-8', newline='') as source:
                rows = list(csv.DictReader(source))
            rows[0].update(gpu_status='available', gpu_us='1500')
            self.write_rows(prefix, '.passes.csv', rows)
            manifest = json.loads(Path(str(prefix) + '.profile.json').read_text(encoding='utf-8'))
            manifest['pass_statistics'][0].update(gpu_us=profile.statistics_for([1500]), gpu_unavailable=0)
            self.write_manifest(prefix, manifest)
            self.assertTrue(profile.validate(prefix, require_gpu=True, require_frame_gpu=True)['verified'])

    def test_legacy_capture_cannot_claim_frame_measurements(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, _, _ = self.write_capture(Path(directory))
            self.assertTrue(profile.validate(prefix)['verified'])
            with self.assertRaisesRegex(AssertionError, 'Schema v1'):
                profile.validate(prefix, require_frame_gpu=True)

    def test_unsupported_frames_are_explicit_and_never_fake_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, frames, _ = self.write_frame_capture(Path(directory), ('unsupported',))
            self.assertTrue(profile.validate(prefix)['verified'])
            with self.assertRaises(AssertionError):
                profile.validate(prefix, require_frame_gpu=True)
            frames[0]['gpu_frame_ms'] = '0'
            self.write_rows(prefix, '.frames.csv', frames)
            with self.assertRaisesRegex(AssertionError, 'Unavailable GPU frame'):
                profile.validate(prefix)

    def test_requirement_rejects_partially_available_frame_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, _, _ = self.write_frame_capture(Path(directory), ('available', 'unsupported'))
            self.assertTrue(profile.validate(prefix)['verified'])
            with self.assertRaises(AssertionError):
                profile.validate(prefix, require_frame_gpu=True)

    def test_incomplete_invalid_and_stale_frame_records_are_rejected(self):
        cases = [('gpu_status', value) for value in ('pending', 'disabled', 'dropped', 'discarded', 'error', 'unknown')]
        cases += [('gpu_frame_ms', value) for value in ('', 'nan', 'inf', '-0.5')]
        cases += [('gpu_intervals', '0'), ('gpu_intervals', '-1'), ('gpu_excluded_intervals', '-1'),
                  ('succeeded', 'false'), ('phase_frame', '1'), ('phase', 'warmup'), ('run_id', 'old_capture')]
        with tempfile.TemporaryDirectory() as directory:
            prefix, frames, _ = self.write_frame_capture(Path(directory))
            for field, value in cases:
                with self.subTest(field=field, value=value):
                    self.write_rows(prefix, '.frames.csv', [dict(frames[0], **{field: value})])
                    with self.assertRaises(AssertionError):
                        profile.validate(prefix)

    def test_missing_duplicate_and_reordered_frames_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, frames, manifest = self.write_frame_capture(Path(directory), ('available',) * 3)
            for changed in (frames[:2], [frames[0], frames[0], frames[2]],
                            [frames[0], frames[2], frames[1]]):
                with self.subTest(frames=changed):
                    self.write_rows(prefix, '.frames.csv', changed)
                    manifest['frame_count'] = len(changed)
                    self.write_manifest(prefix, manifest)
                    with self.assertRaises(AssertionError):
                        profile.validate(prefix)

    def test_stale_frame_statistics_and_dropped_capture_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            for mutation in ('duration', 'unavailable', 'dropped', 'missing_summary'):
                with self.subTest(mutation=mutation):
                    prefix, _, manifest = self.write_frame_capture(Path(directory))
                    if mutation == 'duration':
                        manifest['gpu_frame_ms']['cold']['p95'] = 2
                    elif mutation == 'unavailable':
                        manifest['gpu_frame_unavailable']['cold'] = 1
                    elif mutation == 'dropped':
                        manifest['dropped']['gpu_frames'] = 1
                    else:
                        del manifest['gpu_frame_ms']['measured']
                    self.write_manifest(prefix, manifest)
                    with self.assertRaises(AssertionError):
                        profile.validate(prefix)

    def test_cone_frame_intervals_and_method(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, frames, manifest = self.write_frame_capture(Path(directory), ('available',) * 3)
            manifest['schema_version'] = 4
            for i, frame in enumerate(frames):
                frame.update(gi_method='cone', frame_start_ms=str(i * 10), frame_end_ms=str(i * 10 + 9))
            self.write_rows(prefix, '.frames.csv', frames)
            self.write_manifest(prefix, manifest)
            self.assertTrue(profile.validate(prefix)['verified'])
            for frame in frames:
                frame['gi_method'] = 'hybrid'
            self.write_rows(prefix, '.frames.csv', frames)
            self.assertTrue(profile.validate(prefix)['verified'])
            frames[1]['frame_start_ms'] = '8'
            self.write_rows(prefix, '.frames.csv', frames)
            with self.assertRaisesRegex(AssertionError, 'frame wall intervals'):
                profile.validate(prefix)

    def test_warmup_and_delayed_gi_activation_frame_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix, frames, manifest = self.write_frame_capture(Path(directory), ('available',) * 5)
            manifest.update(requested_frames=3, requested_warmup_frames=2, gi_start_frame=1)
            for frame, phase, local in zip(frames, ('cold', 'warmup', 'warmup', 'cold', 'measured'), (0, 1, 0, 1, 2)):
                frame.update(phase=phase, phase_frame=str(local))
            manifest['cpu_frame_ms'] = {
                phase: profile.statistics_for([2 for frame in frames if frame['phase'] == phase])
                for phase in profile.PHASES}
            manifest['gpu_frame_ms'] = {
                phase: profile.statistics_for([1.5 for frame in frames if frame['phase'] == phase])
                for phase in profile.PHASES}
            self.write_rows(prefix, '.frames.csv', frames)
            self.write_manifest(prefix, manifest)
            self.assertTrue(profile.validate(prefix, require_frame_gpu=True)['verified'])


if __name__ == '__main__':
    unittest.main()
