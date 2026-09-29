"""Portable M8 benchmark: serial config ownership, completed GPU readiness, and paired throughput."""
import argparse
import contextlib
import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time
from urllib.parse import unquote, urlparse

import validate_engine_profile as profile

ROOT = Path(__file__).resolve().parents[1]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf-8')


def file_identity(path):
    path = path.resolve(strict=True)
    return dict(path=path.as_posix(), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def asset_identity(path):
    path = path.resolve(strict=True)
    raw = path.read_bytes()
    if path.suffix.lower() == '.glb':
        assert raw[:4] == b'glTF' and len(raw) >= 20, 'Invalid GLB header'
        size, kind = struct.unpack_from('<II', raw, 12)
        assert kind == 0x4e4f534a, 'Missing GLB JSON chunk'
        document = json.loads(raw[20:20 + size])
    else:
        document = json.loads(raw)
    resources = set()
    for entry in document.get('buffers', []) + document.get('images', []):
        uri = entry.get('uri', '')
        if uri and not uri.startswith('data:'):
            parsed = urlparse(uri)
            assert not parsed.scheme and not parsed.netloc, 'Remote asset URI cannot be fingerprinted'
            resources.add((path.parent / unquote(parsed.path)).resolve(strict=True))
    return dict(scope='main glTF/GLB plus every external buffer and image URI; data URIs are in the document',
                document=file_identity(path), resources=[file_identity(p) for p in sorted(resources)])


def implementation_identity(executable, shaders):
    binaries = sorted(shaders.rglob('*.spv'))
    assert binaries, 'No compiled shaders found'
    return dict(executable=file_identity(executable), shaders=[file_identity(p) for p in binaries])


def profile_file_identity(path):
    """Match the engine export fingerprint, independently of the SHA-256 manifest."""
    path = path.resolve(strict=True)
    data = path.read_bytes()
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return dict(path=path.as_posix(), readable=True, bytes=len(data), fnv1a64=f'{value:016x}')


def profile_input_identity(args):
    shaders = sorted(args.shaders.rglob('*.spv'))
    assert shaders, 'No compiled shaders found'
    return dict(executable=profile_file_identity(args.executable),
                configuration=profile_file_identity(args.config),
                scene_document=profile_file_identity(args.asset),
                shaders=[profile_file_identity(path) for path in shaders])


def checked_profile_fingerprint(entry):
    assert isinstance(entry, dict) and entry.get('readable') is True, 'Unreadable profile input'
    return Path(entry['path']).resolve(), entry['bytes'], entry['fnv1a64']


def validate_profile_inputs(actual, expected):
    assert actual['algorithm'] == 'fnv1a64', 'Unsupported profile fingerprint algorithm'
    assert actual['shader_enumeration_complete'] is True, 'Incomplete shader enumeration'
    for name in ('executable', 'configuration', 'scene_document'):
        assert checked_profile_fingerprint(actual[name]) == checked_profile_fingerprint(expected[name]), (
            'Profile input differs from benchmark input', name)
    captured = [checked_profile_fingerprint(entry) for entry in actual['shaders']]
    declared = [checked_profile_fingerprint(entry) for entry in expected['shaders']]
    assert len({entry[0] for entry in captured}) == len(captured), 'Duplicate captured shader'
    assert captured and set(captured) == set(declared), 'Profile shaders differ from benchmark shaders'


def shader_local_size(path):
    raw = path.read_bytes()
    assert len(raw) >= 20 and len(raw) % 4 == 0, 'Malformed SPIR-V module'
    words = struct.unpack('<' + str(len(raw) // 4) + 'I', raw)
    assert words[0] == 0x07230203, 'Invalid SPIR-V magic'
    offset = 5
    sizes = []
    while offset < len(words):
        count, opcode = words[offset] >> 16, words[offset] & 0xffff
        assert count > 0 and offset + count <= len(words), 'Truncated SPIR-V instruction'
        if opcode == 16 and count == 6 and words[offset + 2] == 17:
            sizes.append(list(words[offset + 3:offset + 6]))
        offset += count
    assert len(sizes) == 1 and all(sizes[0]), 'Expected one literal LocalSize execution mode'
    return sizes[0]


def environment_identity(config, configuration):
    values = {}
    for line in config.decode('utf-8-sig').splitlines():
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    name = values.get('environment_texture', 'papermill.ktx')
    return file_identity(configuration.parent / 'Textures' / name)


def replace_bytes(path, data):
    descriptor, temporary = tempfile.mkstemp(prefix='.m8-config-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as target:
            target.write(data)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


@contextlib.contextmanager
def config_lease(path, edited, output):
    """Never overwrite concurrent user edits; keep the original bytes in the artifact directory."""
    original = path.read_bytes()
    lock = Path(str(path) + '.m8.lock')
    descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    try:
        os.write(descriptor, json.dumps(dict(pid=os.getpid(), output=str(output))).encode())
        os.close(descriptor)
        descriptor = None
        (output / 'original.cfg').write_bytes(original)
        assert path.read_bytes() == original, 'Configuration changed before ownership'
        replace_bytes(path, edited)
        try:
            yield
        finally:
            assert path.read_bytes() == edited, 'Configuration changed externally; preserving it; original.cfg retained'
            replace_bytes(path, original)
    finally:
        if descriptor is not None:
            os.close(descriptor)
        lock.unlink()


def run_command(command, folder, validation, timeout):
    folder.mkdir(parents=True, exist_ok=False)
    write_json(folder / 'command.json', command)
    environment = os.environ.copy()
    environment.update(DISABLE_RTSS_LAYER='1', VK_LOADER_LAYERS_DISABLE='~implicit~')
    if validation:
        environment['VK_LAYER_VALIDATE_SYNC'] = '1'
    start = time.perf_counter()
    with (folder / 'run.log').open('w', encoding='utf-8') as log:
        process = subprocess.run(command, cwd=ROOT, env=environment, stdout=log,
                                 stderr=subprocess.STDOUT, timeout=timeout)
    text = (folder / 'run.log').read_text(encoding='utf-8', errors='replace')
    errors = [line for line in text.splitlines() if any(token in line for token in (
        '[error]', 'VUID-', 'SYNC-HAZARD'))]
    assert process.returncode == 0 and not errors, (process.returncode, errors[:5], str(folder))
    assert '[OK] No memory leaks detected' in text, 'Missing successful allocator shutdown'
    memory = re.findall(r'GPU memory VMA: peak_committed_bytes=(\d+) peak_device_local_bytes=(\d+) remaining_bytes=(\d+)', text)
    assert len(memory) == 1 and int(memory[0][2]) == 0, ('Missing/invalid VMA peak', memory)
    return dict(process_wall_seconds=time.perf_counter() - start,
                vma_peak_committed_bytes=int(memory[0][0]),
                vma_peak_device_local_bytes=int(memory[0][1]), memory_scope=
                'allocator lifetime committed blocks including retained pools; excludes driver/private/swapchain; not residency')


def throughput_samples(path, count):
    with path.open(newline='', encoding='utf-8') as source:
        rows = list(csv.DictReader(source))
    assert len(rows) == count, 'Missing throughput frames'
    assert [int(row['frame']) for row in rows] == list(range(count)), 'Repeated/stale throughput frames'
    values = [float(row['cpu_frame_ms']) for row in rows]
    assert all(math.isfinite(value) and value > 0 for value in values)
    return dict(cpu_frame_ms=profile.statistics_for(values), fps=1000 * len(values) / sum(values))


def traversal_samples(prefix):
    metadata = json.loads(Path(str(prefix) + '.traversal.json').read_text(encoding='utf-8'))
    queries = Path(str(prefix) + '.queries.bin').read_bytes()
    results = Path(str(prefix) + '.query-results.bin').read_bytes()
    count = metadata['queries']
    assert count > 0 and len(queries) == 48 * count and len(results) == 112 * count
    assert metadata['fallback_flags'] == 0, 'Traversal diagnostic captured fallback'
    groups = {}
    for index in range(count):
        mask = struct.unpack_from('<I', queries, 48 * index + 32)[0]
        closest = struct.unpack_from('<I', results, 112 * index + 80)[0]
        occlusion, visits, occlusion_visits, reserved = struct.unpack_from('<4I', results, 112 * index + 96)
        assert mask in (1, 2, 3) and closest in (0, 1, 2) and occlusion in (0, 1, 2) and reserved == 0
        group = groups.setdefault(str(mask), dict(queries=0, closest_cells=0, occlusion_cells=0, unknown=0))
        group['queries'] += 1
        group['closest_cells'] += visits
        group['occlusion_cells'] += occlusion_visits
        group['unknown'] += closest == 0 or occlusion == 0
    return dict(metadata=metadata, class_masks=groups,
                scope='sampled deterministic receiver queries; closest and occlusion visits counted separately; not timed frame totals')


def common_command(args, frames, warmup):
    command = [str(args.executable), '--mode=3', '--fixed-step', '--disable-rt', '--gpu-memory-stats',
               f'--frames={frames}', f'--warmup={warmup}', f'--width={args.width}',
               f'--height={args.height}', f'--gbuffer-size={args.gbuffer}', f'--rhi-thread={args.thread}',
               f'--async-compute={args.async_compute}', f'--vsync={args.vsync}']
    if not args.validation:
        command.append('--disable-validation')
    if args.workload == 'motion':
        command.append('--gi-motion-fixture')
    return command


def check_profile(args, prefix, measured=True, expected_inputs=None):
    result = profile.validate(prefix, require_gpu=True, require_frame_gpu=True,
                              require_ready_gi=measured and args.method == 'dynamic_voxel' and not args.expect_fallback)
    manifest = json.loads(Path(str(prefix) + '.profile.json').read_text(encoding='utf-8'))
    assert manifest['schema_version'] >= 3, 'Build with GPU GI readiness diagnostics'
    assert args.allow_debug or manifest['build']['ndebug'], 'Performance runs require an optimized build'
    validate_profile_inputs(manifest['inputs'],
                            expected_inputs if expected_inputs is not None else profile_input_identity(args))
    assert manifest['settings']['rhi_threaded'] == bool(args.thread), 'Thread override was not applied'
    assert manifest['settings']['voxel_resolution'] == args.resolution
    assert manifest['settings']['voxelizer'] == args.voxelizer
    assert manifest['settings']['vsync_requested'] == bool(args.vsync)
    if args.method == 'dynamic_voxel' and not args.expect_fallback:
        assert manifest['settings']['rays_per_face'] == args.rays
        assert manifest['settings']['cache'] == args.cache
        if not measured:
            assert result['gi']['epochs'] and all(epoch['first_ready_frame'] is not None
                for epoch in result['gi']['epochs']), 'Cold capture ended before DDA became ready'
    if args.require_async:
        assert manifest['settings']['async_compute_status'] == 'available', 'Dedicated async compute unavailable'
    with Path(str(prefix) + '.frames.csv').open(newline='') as source:
        measured_rows = [row for row in csv.DictReader(source) if row['phase'] == 'measured']
    if measured:
        assert measured_rows, 'No measured frames'
        expected = 'cone' if args.method == 'cone' else 'dynamic_voxel'
        if args.expect_fallback:
            assert all(row['gi_effective_method'] != 'dynamic_voxel' for row in measured_rows)
        else:
            assert all(row['gi_effective_method'] == expected for row in measured_rows), 'Unexpected method fallback'
    return dict(verification=result, device=manifest['device'], settings=manifest['settings'],
                gpu_frame_ms=manifest['gpu_frame_ms'], cpu_frame_ms=manifest['cpu_frame_ms'],
                pass_statistics=manifest['pass_statistics'])


def benchmark(args):
    args.output.mkdir(parents=True, exist_ok=False)
    base = args.base_config.read_bytes() if args.base_config else args.config.read_bytes()
    settings = dict(default_model_path=args.asset.as_posix(), voxel_gi_method=args.method,
                    voxel_resolution=args.resolution, voxelizer=args.voxelizer,
                    dynamic_voxel_gi_query_backend='voxel_dda', dynamic_voxel_gi_cache=args.cache,
                    dynamic_voxel_gi_rays_per_face=args.rays, dynamic_voxel_gi_memory_budget_mb=args.budget_mb,
                    **{'dynamic_light.enabled': str(args.workload == 'lights').lower()})
    for item in args.set:
        key, value = item.split('=', 1)
        settings[key] = value
    edited = base + b'\n' + ''.join(f'{key}={value}\n' for key, value in settings.items()).encode()
    identity = implementation_identity(args.executable, args.shaders)
    manifest = dict(schema_version=1, created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    implementation=identity, asset=asset_identity(args.asset), settings=settings,
                    environment=environment_identity(edited, args.config),
                    runner=file_identity(Path(__file__)), validator=file_identity(Path(profile.__file__)),
                    arguments={key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
                    config_sha256=hashlib.sha256(edited).hexdigest(), complete=False, trials=[])
    manifest['gather_workgroups_from_spirv'] = {name: shader_local_size(
        args.shaders / 'VoxelGI/Dynamic' / (name + '.comp.spv')) for name in (
        'static_gather', 'frame_gather_reference', 'frame_gather_environment_dda',
        'frame_gather_environment_reference')}
    write_json(args.output / 'manifest.json', manifest)
    (args.output / 'engine.cfg').write_bytes(edited)
    run_ids = set()
    with config_lease(args.config, edited, args.output):
        # Freeze the exact declared inputs before launching any capture. A --set
        # scene override or a different compiled shader directory must not be accepted.
        manifest['profile_inputs'] = profile_input_identity(args)
        write_json(args.output / 'manifest.json', manifest)
        for trial in range(args.trials):
            record = dict(trial=trial)
            if args.cold_frames:
                folder = args.output / f'trial-{trial:02d}-cold'
                prefix = folder / 'capture'
                command = common_command(args, args.cold_frames, 0) + [f'--profile={prefix}', '--gi-start-frame=1']
                record['cold'] = run_command(command, folder, args.validation, args.timeout)
                record['cold'].update(check_profile(args, prefix, measured=False,
                                                     expected_inputs=manifest['profile_inputs']))
                run_ids.add(record['cold']['verification']['run_id'])
            folder = args.output / f'trial-{trial:02d}-profile'
            prefix = folder / 'capture'
            command = common_command(args, args.frames, args.warmup) + [f'--profile={prefix}']
            record['profile'] = run_command(command, folder, args.validation, args.timeout)
            record['profile'].update(check_profile(args, prefix, expected_inputs=manifest['profile_inputs']))
            run_id = record['profile']['verification']['run_id']
            assert run_id not in run_ids, 'Repeated profile run'
            run_ids.add(run_id)
            if 'cold' in record:
                assert record['cold']['device'] == record['profile']['device'], 'GPU changed between captures'
            if manifest['trials']:
                assert manifest['trials'][0]['profile']['device'] == record['profile']['device'], 'GPU changed between trials'
            if not args.no_throughput:
                folder = args.output / f'trial-{trial:02d}-throughput'
                path = folder / 'frames.csv'
                command = common_command(args, args.frames, args.warmup) + [f'--frame-times={path}']
                record['throughput'] = run_command(command, folder, args.validation, args.timeout)
                record['throughput'].update(throughput_samples(path, args.frames))
                record['throughput']['scope'] = 'uninstrumented CPU frame wall time including frame-slot backpressure; not GPU active time'
            if args.diagnostics and args.method == 'dynamic_voxel' and not args.expect_fallback:
                folder = args.output / f'trial-{trial:02d}-traversal'
                command = common_command(args, args.frames, args.warmup) + [f'--capture-traversal={folder / "capture"}']
                record['traversal'] = run_command(command, folder, args.validation, args.timeout)
                record['traversal'].update(traversal_samples(folder / 'capture'))
                record['traversal']['scope'] = 'separate intrusive diagnostic run; excluded from timed aggregates'
            assert args.config.read_bytes() == edited, 'Configuration changed during trial'
            assert implementation_identity(args.executable, args.shaders) == identity, 'Implementation changed during trial'
            assert asset_identity(args.asset) == manifest['asset'], 'Asset changed during trial'
            assert environment_identity(edited, args.config) == manifest['environment'], 'Environment changed during trial'
            manifest['trials'].append(record)
            write_json(args.output / 'manifest.json', manifest)
            print(f'Finished trial {trial + 1}/{args.trials}: {args.output.name}', flush=True)
    medians = [trial['profile']['gpu_frame_ms']['measured']['median'] for trial in manifest['trials']]
    manifest['gpu_median_variation_ms'] = profile.statistics_for(medians)
    manifest['complete'] = True
    write_json(args.output / 'manifest.json', manifest)
    return manifest


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--asset', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--config', type=Path, default=ROOT / 'Data/engine.cfg', help='Engine configuration file to own temporarily')
    parser.add_argument('--base-config', type=Path)
    parser.add_argument('--shaders', type=Path, default=ROOT / 'Data/SpvShaders')
    parser.add_argument('--method', choices=('dynamic_voxel', 'cone'), default='dynamic_voxel')
    parser.add_argument('--voxelizer', choices=('comp', 'geom'), default='comp')
    parser.add_argument('--resolution', type=int, choices=(64, 128), default=64)
    parser.add_argument('--rays', type=int, choices=(32, 64, 128), default=128)
    parser.add_argument('--cache', choices=('compact', 'decoded'), default='compact')
    parser.add_argument('--budget-mb', type=int, default=6144)
    parser.add_argument('--workload', choices=('static', 'motion', 'lights'), default='static')
    parser.add_argument('--frames', type=int, default=120)
    parser.add_argument('--warmup', type=int, default=96)
    parser.add_argument('--cold-frames', type=int, default=64)
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--width', type=int, default=320)
    parser.add_argument('--height', type=int, default=180)
    parser.add_argument('--gbuffer', type=int, default=256)
    parser.add_argument('--thread', type=int, choices=(0, 1), default=1)
    parser.add_argument('--async-compute', type=int, choices=(0, 1), default=1)
    parser.add_argument('--vsync', type=int, choices=(0, 1), default=0)
    parser.add_argument('--require-async', action='store_true')
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--allow-debug', action='store_true')
    parser.add_argument('--expect-fallback', action='store_true')
    parser.add_argument('--diagnostics', action='store_true')
    parser.add_argument('--no-throughput', action='store_true')
    parser.add_argument('--timeout', type=int, default=1800)
    parser.add_argument('--set', action='append', default=[], metavar='KEY=VALUE')
    args = parser.parse_args()
    if min(args.frames, args.trials, args.width, args.height, args.gbuffer, args.budget_mb, args.timeout) <= 0:
        parser.error('Counts, dimensions, budget and timeout must be positive')
    if args.warmup < 0 or args.cold_frames not in (0,) and args.cold_frames < 2:
        parser.error('Warmup must be nonnegative; cold capture requires at least two frames')
    if any('=' not in item or '\n' in item or '\r' in item for item in args.set):
        parser.error('Overrides must be single KEY=VALUE lines')
    for name in ('executable', 'asset', 'output', 'config', 'shaders', 'base_config'):
        if getattr(args, name) is not None:
            setattr(args, name, getattr(args, name).resolve())
    return args


if __name__ == '__main__':
    result = benchmark(parse_arguments())
    print(json.dumps(dict(complete=result['complete'], gpu_median_variation_ms=result['gpu_median_variation_ms']), indent=2))
