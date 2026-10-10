"""Compare fresh engine captures of the frozen Sponza views with the stored sky ground truth.

The ground-truth dataset (default build/ground-truth/dataset-p4-corrected, ignored by Git) holds one Mitsuba
render per environment and camera, truth-<environment>-<camera>.npz/.json, the fixture-gate report
the renders were validated with, and dataset.json. For each view, dataset.json records what the
render depends on: the scene and environment file hashes, the camera matrix and resolution, and
the engine's captured level-zero environment cube. Renders are never overwritten or modified.

Engine captures are not stored with the dataset; this tool regenerates them for the current build
under --output:

  gt/<view>, gt/<view>-reference      normal-map-free shipping and tier-reference captures
  ship/<view>, ship/<view>-reference  normal-mapped captures for the shipping gate

then writes report.json and summary.md: the tier reference and the shipping preset against the
ground truth (rung 3 of the plan's ground-truth ladder, per pixel, on 8x8 blocks and near/far from
the camera) and the shipping gate (shipping against the tier reference, normal maps on) with its
temporal/filtered error attribution. A capture whose camera, scene or environment cube differs from
the render it would be compared with is an error: that ground truth no longer applies.

--render-missing renders the ground truth for requested views that are not in the dataset yet
(Mitsuba, CUDA where available), several at a time because the renderer is latency-bound. It needs
a passing fixture gate from 'ground_truth_mitsuba.py --fixtures'. --import-truth registers a
render made earlier by ground_truth_mitsuba.py from a normal-map-free capture.

Run with the Python environment from tools/requirements-gi-quality.txt. Captures mutate
Data/engine.cfg transactionally and run one at a time.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_hybrid_gi import compare as shipping_gate, load as load_capture  # noqa: E402
from capture_hybrid_gi import CAMERAS as CAMERA_MATRICES  # noqa: E402
from ground_truth_mitsuba import compare as compare_truth, receiver_agreement  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CAMERAS = ('top', 'hall')
# Environment names used in reports, and their paths relative to Data/Textures.
ENVIRONMENTS = {
    'papermill': 'papermill.ktx',
    'hotel': 'Environments/hotel_room_1k.hdr',
    'kloppenheim': 'Environments/kloppenheim_06_puresky_1k.hdr',
    'kloofendal': 'Environments/kloofendal_48d_partly_cloudy_puresky_1k.hdr',
    'qwantani': 'Environments/qwantani_noon_puresky_2k.hdr',
    'studio': 'Environments/studio_small_09_1k.hdr',
    'emptyroom': 'Environments/small_empty_room_1_1k.hdr',
    'corridor': 'Environments/large_corridor_1k.hdr',
    'carpentry': 'Environments/carpentry_shop_01_1k.hdr',
}
LUMINANCE = np.array([.2126, .7152, .0722])
# Rung-3 limits for the tier reference and the P0 shipping limits.
TIER_BIAS, TIER_EXCESS = .01, .03


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def log(text):
    print(time.strftime('%H:%M:%S ') + text, flush=True)


def capture(args, prefix, environment, camera, strip, reference):
    command = [sys.executable, str(ROOT / 'tools/capture_hybrid_gi.py'), '--exe', str(args.exe), '--scene', str(args.scene),
               '--output', str(prefix), '--camera', camera, '--provider', 'hardware', '--rt',
               '--environment-texture', ENVIRONMENTS[environment]]
    if strip:
        command.append('--strip-normal-maps')
    if reference:
        command += ['--reference-samples', '1024']
    if args.reuse_captures and Path(str(prefix) + '.inputs.json').exists():
        inputs = json.loads(Path(str(prefix) + '.inputs.json').read_text())
        files = [inputs['executable']] + inputs['shaders'] + inputs['source_inputs']
        stale = [entry['path'] for entry in files
                 if not Path(entry['path']).exists() or sha256(entry['path']) != entry['sha256']]
        missing = [str(prefix)+suffix for suffix in ('.lighting.json', '.hybrid.bin', '.environment.bin', '.gltf')
                   if not Path(str(prefix)+suffix).exists()]
        scene = json.loads(Path(str(prefix)+'.gltf').read_text()) if not missing else {}
        metadata = json.loads(Path(str(prefix)+'.lighting.json').read_text()) if not missing else {}
        has_normals = any('normalTexture' in material for material in scene.get('materials', []))
        source = json.loads(args.scene.read_text(encoding='utf-8'))
        expected_normals = not strip and any('normalTexture' in material for material in source.get('materials', []))
        settings = inputs['settings']
        expected = dict(environment_texture=ENVIRONMENTS[environment], voxel_gi_reference_samples=1024 if reference else 0,
                        voxel_gi_ray_provider='hardware', voxel_gi_samples=4, voxel_gi_history_frames=32,
                        voxel_gi_indirect_intensity=0, voxel_resolution=64, voxel_gi_temporal='true', voxel_gi_filter='true')
        nodes = scene.get('nodes', [])
        wrong_settings = (any(settings.get(key) != value for key, value in expected.items())
                          or has_normals != expected_normals
                          or not nodes or nodes[-1].get('matrix') != CAMERA_MATRICES[camera]
                          or any(metadata.get(key) != value for key, value in
                                 dict(width=960, height=540, frame=64, ray_provider='hardware').items())
                          or sha256(args.scene) not in {entry['sha256'] for entry in inputs['source_inputs']}
                          or inputs['executable']['sha256'] != sha256(args.exe))
        if stale or missing or wrong_settings:
            raise SystemExit(f'Refusing stale/incomplete capture {prefix}: files={stale + missing}, settings={wrong_settings}')
        return
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise SystemExit(f'Capture {prefix.name} failed:\n{result.stdout[-2000:]}')
    log(f'captured {prefix.name}')


def identity(prefix):
    """What a ground-truth render of this capture depends on."""
    metadata = json.loads(Path(str(prefix) + '.lighting.json').read_text())
    inputs = json.loads(Path(str(prefix) + '.inputs.json').read_text())
    scene = json.loads(Path(str(prefix) + '.gltf').read_text())
    environment = Path(inputs['settings']['environment_texture'])
    environment = environment if environment.is_absolute() else ROOT / 'Data/Textures' / environment
    environment_hash = next(i['sha256'] for i in inputs['source_inputs'] if Path(i['path']) == environment.resolve())
    result = dict(
        width=metadata['width'], height=metadata['height'],
        projection_view_column_major=metadata['projection_view_column_major'],
        environment_texture=inputs['settings']['environment_texture'], environment_sha256=environment_hash,
        environment_intensity_rotation_enabled_visible=metadata['environment_intensity_rotation_enabled_visible'],
        environment_cube_size=metadata['environment_cube_size'],
        environment_cube_sha256=sha256(str(prefix) + '.environment.bin'),
        scene_sha256=sorted(i['sha256'] for i in inputs['source_inputs'] if i['sha256'] != environment_hash),
        normal_maps=any('normalTexture' in m for m in scene.get('materials', [])))
    if result['normal_maps']:
        _, data = load_capture(prefix)
        result['shading_normals_sha256'] = hashlib.sha256(data[:, :, 9, :3].copy().tobytes()).hexdigest()
    return result


def check_identity(view, entry, prefix):
    actual = identity(prefix)
    problems = [key for key in ('width', 'height', 'environment_sha256', 'environment_cube_size', 'environment_cube_sha256',
                                'scene_sha256', 'normal_maps', 'environment_intensity_rotation_enabled_visible')
                if actual[key] != entry[key]]
    if not np.allclose(actual['projection_view_column_major'], entry['projection_view_column_major'], rtol=0, atol=1e-7):
        problems.append('projection_view_column_major')
    if entry['normal_maps'] and actual['shading_normals_sha256'] != entry.get('shading_normals_sha256'):
        problems.append('shading_normals_sha256')
    if problems:
        raise SystemExit(f'{view}: capture {prefix} does not match its ground truth ({", ".join(problems)}). '
                         'The stored render no longer applies; render a new dataset instead of comparing.')


def load_dataset(folder):
    path = folder / 'dataset.json'
    return json.loads(path.read_text()) if path.exists() else dict(views={}, fixture_gates=[])


def save_dataset(folder, dataset):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'dataset.json').write_text(json.dumps(dataset, indent=2) + '\n')


def register(folder, dataset, view, truth_prefix, gate):
    """Copy a ground_truth_mitsuba.py render into the dataset unchanged and record what it depends on."""
    truth_prefix = Path(truth_prefix)
    if view in dataset['views'] or (folder / f'truth-{view}.npz').exists():
        raise SystemExit(f'{view} is already in the dataset; stored renders are never replaced')
    render = json.loads(Path(str(truth_prefix) + '.json').read_text())
    source = Path(render['capture'])
    source = source if source.is_absolute() else ROOT / source
    entry = identity(source)
    if render.get('receivers', 'exact') != 'exact':
        raise SystemExit(f'{view}: diagnostic engine receivers cannot be registered as independent ground truth')
    if entry['normal_maps'] and render.get('shading_normals') != 'engine':
        raise SystemExit(f'{view}: normal-mapped captures require ground truth rendered with --shading-normals engine')
    for suffix in ('.npz', '.json'):
        shutil.copyfile(str(truth_prefix) + suffix, folder / f'truth-{view}{suffix}')
    report = Path(str(truth_prefix) + '-compare.json')
    entry.update(truth=f'truth-{view}.npz', truth_sha256=sha256(folder / f'truth-{view}.npz'), render=render,
                 embree_cross_check=json.loads(report.read_text()).get('embree_cross_check') if report.exists() else None,
                 fixture_gate=gate, registered=time.strftime('%Y-%m-%d'))
    dataset['views'][view] = entry
    save_dataset(folder, dataset)
    log(f'registered {view}')


def archive_gate(folder, dataset, gate):
    """Archive a passing fixture-gate report with the dataset; returns its archived name."""
    results = json.loads(Path(gate).read_text())
    failed = [name for name, entry in results.items() if not entry.get('passed')]
    if failed:
        raise SystemExit(f'Fixture gate {gate} has failures: {", ".join(failed)}')
    digest = sha256(gate)
    name = f'fixture-gate-{digest[:12]}.json'
    folder.mkdir(parents=True, exist_ok=True)
    if not (folder / name).exists():
        shutil.copyfile(gate, folder / name)
        dataset['fixture_gates'].append(dict(file=name, source=str(gate), sha256=digest, fixtures=len(results)))
    return name


def render(args, view):
    prefix = args.output / 'gt' / view
    output = args.output / 'truth' / f'truth-{view}'
    output.parent.mkdir(parents=True, exist_ok=True)
    start = time.time()
    log(f'rendering ground truth {view}')
    result = subprocess.run([sys.executable, str(ROOT / 'tools/ground_truth_mitsuba.py'), str(prefix), '--output', str(output),
                             '--samples', str(args.samples), '--batch', str(args.batch), '--seed', str(args.seed),
                             '--shading-normals', 'engine' if args.normal_maps else 'vertex',
                             '--backend', args.backend], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    Path(str(output) + '.log').write_text(result.stdout)
    if result.returncode != 0:
        raise SystemExit(f'Ground truth {view} failed; see {output}.log')
    log(f'rendered ground truth {view} in {time.time() - start:.0f} s')
    return output


def attribution(candidate, reference):
    """Luminance error of the temporal stage (its unfiltered mean is the first moment) and of the filtered
    output, and the share of the largest filtered errors on pixels whose neighbors mostly differ in
    geometric normal (fine geometry)."""
    _, c = load_capture(candidate)
    _, r = load_capture(reference)
    valid = r[:, :, 8, 3] > 0
    floor = valid & (r[:, :, 10, 1] > .99) & (r[:, :, 8, 1] < -.12)
    expected, temporal, filtered = r[:, :, 1, :3] @ LUMINANCE, c[:, :, 6, 0], c[:, :, 1, :3] @ LUMINANCE
    normals = np.pad(c[:, :, 10, :3], ((1, 1), (1, 1), (0, 0)), mode='edge')
    height, width = valid.shape
    similar = sum((np.sum(c[:, :, 10, :3] * normals[1 + dy:1 + dy + height, 1 + dx:1 + dx + width], -1) > .9)
                  for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dx or dy)
    fine = valid & (similar <= 2)
    report = {}
    for name, mask in (('all_receivers', valid), ('sponza_floor', floor)):
        mean = expected[mask].mean()
        t, f = temporal[mask] - expected[mask], filtered[mask] - expected[mask]
        report[name] = dict(temporal_bias=float(t.mean() / mean), temporal_rms=float(np.sqrt((t ** 2).mean()) / mean),
                            filtered_bias=float(f.mean() / mean), filtered_rms=float(np.sqrt((f ** 2).mean()) / mean))
    error = np.abs(filtered - expected)
    largest = valid & (error >= np.quantile(error[valid], .99))
    report['fine_geometry'] = dict(pixel_share=float(fine.sum() / valid.sum()),
                                   largest_error_share=float((largest & fine).sum() / largest.sum()))
    return report


def compare_view(args, dataset, view):
    environment, camera = view.rsplit('-', 1)
    entry = dataset['views'][view]
    truth = args.dataset / entry['truth']
    if sha256(truth) != entry['truth_sha256']:
        raise SystemExit(f'{truth} has changed since it was registered')
    result = {}
    shipping, reference = args.output / 'gt' / view, args.output / 'gt' / f'{view}-reference'
    for prefix in (shipping, reference):
        check_identity(view, entry, prefix)
    result['ground_truth'] = dict(reference=compare_truth(truth, reference), shipping=compare_truth(truth, shipping),
                                  truth_samples=entry['render']['samples'])
    metadata, data = load_capture(reference)
    covered, _, agree = receiver_agreement(np.load(truth), metadata, data)
    if not args.skip_shipping_gate:
        shipping, reference = args.output / 'ship' / view, args.output / 'ship' / f'{view}-reference'
        capture(args, reference, environment, camera, False, True)
        capture(args, shipping, environment, camera, False, False)
        result['shipping_gate'] = shipping_gate(shipping, reference, {'primary_mismatch_receivers': covered & ~agree})
        result['attribution'] = attribution(shipping, reference)
    return result


def percent(value, signed=False):
    return '-' if value is None else (f'{100 * value:+.2f}%' if signed else f'{100 * value:.2f}%')


def tier_verdict(reference):
    """The plan's rung-3 rule: an excess above the limit fails, per pixel or on 8x8 blocks (excess is already
    corrected for the ground truth's noise). Where the per-pixel excess is within the limit but the ground
    truth's per-pixel noise exceeds it, the per-pixel pass cannot be confirmed and the blocks decide."""
    regions = [reference[name] for name in ('all_receivers', 'sponza_floor') if name in reference]
    if any(abs(x['bias']) > TIER_BIAS or x['excess_rms'] > TIER_EXCESS or x['blocks']['excess_rms'] > TIER_EXCESS for x in regions):
        return '**Fail**'
    noisy = [x for x in regions if x['truth_noise_rms'] > TIER_EXCESS]
    if noisy:
        return f'Pass on 8x8 blocks (per-pixel noise {percent(max(x["truth_noise_rms"] for x in noisy))})'
    return 'Pass'


def summary(report):
    lines = ['# Ground-truth sweep', '', f'Build: {report["executable"]["path"]}', '',
             '## Tier reference and shipping against the ground truth', '',
             'Excess is RMS error with the ground truth\'s own noise removed; limits for the tier reference: '
             f'|bias| <= {100 * TIER_BIAS:.0f}%, excess <= {100 * TIER_EXCESS:.0f}% per pixel and on 8x8 blocks.', '',
             '| View | Ground-truth noise | Tier reference: bias / excess / 8x8 excess | Floor: bias / excess | '
             'Near / far excess | Shipping: bias / excess | Tier reference |',
             '| --- | ---: | --- | --- | --- | --- | --- |']
    for view, result in report['views'].items():
        ref, ship = result['ground_truth']['reference'], result['ground_truth']['shipping']
        a, f, d = ref['all_receivers'], ref.get('sponza_floor'), ref['all_receivers']['distance']
        floor = f'{percent(f["bias"], True)} / {percent(f["excess_rms"])}' if f else '-'
        bands = ' / '.join(percent(d[b]['excess_rms']) if b in d else '-' for b in ('near', 'far'))
        lines.append(f'| {view} | {percent(a["truth_noise_rms"])} | {percent(a["bias"], True)} / {percent(a["excess_rms"])} / '
                     f'{percent(a["blocks"]["excess_rms"])} | {floor} | {bands} | '
                     f'{percent(ship["all_receivers"]["bias"], True)} / {percent(ship["all_receivers"]["excess_rms"])} | '
                     f'{tier_verdict(ref)} |')
    gated = {view: result for view, result in report['views'].items() if 'shipping_gate' in result}
    if gated:
        lines += ['', '## Shipping gate (normal maps on, against the tier reference)', '',
                  'Bias / RMS / P99 / exact zeros; limits |bias| <= 2%, RMS <= 8%, P99 <= 20%, no zeros.', '',
                  '| View | All receivers | Floor | Temporal bias / RMS | Filtered bias / RMS | Largest errors on fine geometry |',
                  '| --- | --- | --- | --- | --- | --- |']
        for view, result in gated.items():
            cells = []
            for name in ('all_receivers', 'sponza_floor'):
                g = result['shipping_gate'][name]
                cell = f'{percent(g["bias"], True)} / {percent(g["rms"])} / {percent(g["p99"])} / {g["unexpected_zeros"]}'
                cells.append(cell if g['passed'] else f'**{cell}**')
            t = result['attribution']['all_receivers']
            lines.append(f'| {view} | {cells[0]} | {cells[1]} | {percent(t["temporal_bias"], True)} / {percent(t["temporal_rms"])} | '
                         f'{percent(t["filtered_bias"], True)} / {percent(t["filtered_rms"])} | '
                         f'{percent(result["attribution"]["fine_geometry"]["largest_error_share"])} |')
    lines += ['', '## Primary-surface mismatches', '',
              'Different primary surfaces are excluded from the agreement gate above and reported here separately. '
              'Truth errors compare different surfaces; shipping errors use the same raster receiver and tier reference. '
              'Rays retain level-zero alpha acceptance.', '',
              '| View | Mismatches / covered | Missing truth hits | Reference vs truth: bias / excess | Shipping vs tier: bias / RMS / P99 / zeros |',
              '| --- | --- | --- | --- | --- |']
    for view, result in report['views'].items():
        ref = result['ground_truth']['reference']
        mismatch = ref.get('primary_mismatch_receivers')
        truth_cell = f'{percent(mismatch["bias"], True)} / {percent(mismatch["excess_rms"])}' if mismatch else '-'
        ship = result.get('shipping_gate', {}).get('primary_mismatch_receivers')
        ship_cell = (f'{percent(ship["bias"], True)} / {percent(ship["rms"])} / {percent(ship["p99"])} / '
                     f'{ship["unexpected_zeros"]}') if ship else '-'
        lines.append(f'| {view} | {ref["primary_mismatch"]} / {ref["covered"]} | {ref.get("primary_missing", 0)} | '
                     f'{truth_cell} | {ship_cell} |')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--exe', type=Path, help='scene_renderer_demo executable')
    parser.add_argument('--scene', type=Path, help='glTF Sample Assets Sponza/glTF/Sponza.gltf')
    parser.add_argument('--dataset', type=Path, default=ROOT / 'build/ground-truth/dataset-p4-corrected')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/ground-truth/runs/latest', help='Captures and reports of this run')
    parser.add_argument('--environments', nargs='+', choices=ENVIRONMENTS, default=list(ENVIRONMENTS))
    parser.add_argument('--cameras', nargs='+', choices=CAMERAS, default=list(CAMERAS))
    parser.add_argument('--skip-shipping-gate', action='store_true', help='Only the normal-map-free ground-truth comparison')
    parser.add_argument('--reuse-captures', action='store_true', help='Keep captures already in --output (same build only)')
    parser.add_argument('--render-missing', action='store_true', help='Render ground truth for requested views not in the dataset')
    parser.add_argument('--fixture-gate', type=Path, help='fixtures.json of a passing ground_truth_mitsuba.py --fixtures run')
    parser.add_argument('--workers', type=int, default=4, help='Concurrent ground-truth renders')
    parser.add_argument('--samples', type=int, default=32768)
    parser.add_argument('--batch', type=int, default=32)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--backend', choices=('auto', 'cuda', 'llvm'), default='auto')
    parser.add_argument('--normal-maps', action='store_true',
                        help='Check captured normal-mapped shading normals; requires a separate matching ground-truth dataset')
    parser.add_argument('--import-truth', nargs=2, action='append', metavar=('VIEW', 'TRUTH_PREFIX'), default=[],
                        help='Register an earlier ground_truth_mitsuba.py render as <environment>-<camera>')
    args = parser.parse_args()
    args.dataset, args.output = args.dataset.resolve(), args.output.resolve()
    dataset = load_dataset(args.dataset)

    if args.import_truth:
        assert args.fixture_gate, '--import-truth needs the --fixture-gate the renders were validated with'
        gate = archive_gate(args.dataset, dataset, args.fixture_gate)
        for view, truth in args.import_truth:
            assert view.rsplit('-', 1)[0] in ENVIRONMENTS and view.rsplit('-', 1)[-1] in CAMERAS, view
            register(args.dataset, dataset, view, truth, gate)
        return

    assert args.exe and args.scene, '--exe and --scene are required to capture'
    views = [f'{environment}-{camera}' for environment in args.environments for camera in args.cameras]
    missing = [view for view in views if view not in dataset['views']]
    if missing and not args.render_missing:
        raise SystemExit(f'No ground truth for {", ".join(missing)}; use --render-missing or choose other views')
    gate = None
    if missing:
        assert args.fixture_gate, '--render-missing needs --fixture-gate'
        gate = archive_gate(args.dataset, dataset, args.fixture_gate)
        save_dataset(args.dataset, dataset)

    # Normal-map-free captures first, so missing renders can start while the rest are captured.
    futures = {}
    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        for view in views:
            environment, camera = view.rsplit('-', 1)
            capture(args, args.output / 'gt' / f'{view}-reference', environment, camera, not args.normal_maps, True)
            capture(args, args.output / 'gt' / view, environment, camera, not args.normal_maps, False)
            if view in missing:
                futures[view] = pool.submit(render, args, view)
        for view, future in futures.items():
            register(args.dataset, dataset, view, future.result(), gate)

    report = dict(executable=dict(path=str(args.exe.resolve()), sha256=sha256(args.exe)), dataset=str(args.dataset), views={})
    for view in views:
        report['views'][view] = compare_view(args, dataset, view)
        log(f'compared {view}')
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    (args.output / 'summary.md').write_text(summary(report), encoding='utf-8')
    log(f'wrote {args.output / "summary.md"}')


if __name__ == '__main__':
    main()
