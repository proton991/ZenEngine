"""Production fast-turn capture and settled-pose comparison for native_gi_fast_motion."""
from collections import defaultdict

import numpy as np
from PIL import Image

import gi_quick_gates as gates
from gi_spark_metric import rotate_target, settled_turn_metrics
from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json
from profile_hybrid_gi import actual_state, running_renderers


RECOVERY = (*range(1, 17), 32, 64)


def schedules(setup, kind, degrees, warmup, hold):
    eye, target = gates.base_pose(setup)
    steps = abs(degrees) // 30 if kind == 'sweep' else 1
    angles = [i * degrees // steps for i in range(steps + 1)]

    def key(frame, angle):
        return gates.key(frame, (eye, rotate_target(eye, target, angle) if angle else target))

    frame = warmup
    moving = [key(1, 0), key(frame, 0)]
    rows = []
    for visit, path in [('out', angles), ('return', angles[::-1])]:
        for angle in path[1:]:
            frame += 1
            moving.append(key(frame, angle))
            rows.append(dict(visit=visit, angle=angle, recovery=0, frame=frame))
        for delay in RECOVERY:
            rows.append(dict(visit=visit, angle=path[-1], recovery=delay, frame=frame + delay))
        frame += max(hold, RECOVERY[-1])
        moving.append(key(frame, path[-1]))

    references, ref_rows, frame = [], {}, 1
    for angle in angles:
        # Five reference samples and six disjoint floor samples per phase and pose.
        first = frame + hold
        samples = [first + 16 * i for i in range(11)]
        references.extend((key(frame, angle), key(samples[-1], angle)))
        ref_rows[angle] = dict(mean=samples[:5], floor=samples[5:])
        frame = samples[-1] + 1
    return dict(keys=moving), rows, dict(keys=references), ref_rows


def load_image(folder, frame):
    with Image.open(gates.frame_path(folder, frame)) as image:
        if image.size != (3840, 2160):
            raise ValueError(f'{folder}, frame {frame}: expected native 4K pixels, got {image.size}')
        return np.array(image.convert('RGB'))


def projection_view(folder, frame):
    return read_json(str(gates.frame_path(folder, frame)) + '.json')['projectionViewBits']


def reproject_pixels(ages, previous, current, extent):
    """Fixed-eye rotation maps each old camera ray to a pixel without a depth estimate."""
    if not ages:
        return {}
    height, width = extent
    pixels = list(ages)
    y, x = np.asarray(pixels).T
    clip = np.array([2 * (x + .5) / width - 1, 2 * (y + .5) / height - 1,
                     np.full(len(x), .5), np.ones(len(x))])
    projected = current @ np.linalg.solve(previous, clip)
    visible = (projected[3] > 0) & (np.abs(projected[:2]) < projected[3]).all(axis=0)
    dest = np.floor((projected[:2, visible] / projected[3, visible] + 1) *
                    np.array([[width / 2], [height / 2]])).astype(int)
    result = {}
    for source, (sx, sy) in zip(np.flatnonzero(visible), dest.T):
        result[sy, sx] = max(result.get((sy, sx), 0), ages[pixels[source]])
    return result


def mark_lifetimes(rows, folder):
    """Report held-pixel overlap and fixed-eye ray overlap across consecutive sweep frames."""
    previous = {}
    projected = {}
    last_matrix = None
    last = None
    for row in rows:
        pose = (row['visit'], row['angle'])
        matrix = np.asarray(projection_view(folder, row['frame']), dtype=np.uint32).view(np.float32).reshape(4, 4).T
        if last is None or last[0][0] != row['visit'] or last[1] + 1 != row['frame']:
            projected = {}
        elif last[0] != pose:
            projected = reproject_pixels(projected, last_matrix, matrix, (2160, 3840))
        if last != (pose, row['frame'] - 1):
            previous = {}
        current = {}
        current_projected = {}
        for feature in row['features']:
            pixels = [(y, x) for y, x0, x1 in feature['spans'] for x in range(x0, x1 + 1)]
            age = 1 + max(previous.get(p, 0) for p in pixels)
            feature['observed_frames'] = age
            motion_age = 1 + max(projected.get(p, 0) for p in pixels)
            feature['projected_frames'] = motion_age
            current.update(dict.fromkeys(pixels, age))
            current_projected.update(dict.fromkeys(pixels, motion_age))
        previous, projected, last_matrix = current, current_projected, matrix
        last = (pose, row['frame'])


def capture(args, name, setup, camera, frames):
    folder = args.output / name
    if args.rescore:
        if read_json(folder / 'camera-path.json') != camera or read_json(folder / 'setup.zenrender') != setup:
            raise ValueError(f'Stored camera or setup differs at {folder}')
        command = read_json(folder / 'zenapp.log.command.json')
    else:
        command = gates.launch(args, folder, setup, camera, None, frames, frames[-1],
                               startup=('--fixed-step', '--no-ui', '--backend=hardware', '--gpu-culling=1',
                                        '--gpu-markers'))
    state = actual_state((folder / 'setup.log').read_text(errors='replace'))
    run = setup['run']
    expected = dict(render_width=3840, render_height=2160, gi_width=3840, gi_height=2160,
                    framebuffer_width=3840, framebuffer_height=2160, fullscreen=run['fullscreen'],
                    ray_provider='hardware', gather='screen_probes')
    if any(state[k] != v for k, v in expected.items()):
        raise ValueError(f'{folder}: actual state {state}, expected {expected}')
    capacity = args.cache_cells or read_json(str(gates.frame_path(folder, frames[0])) + '.json')['worldCacheCapacity']
    if capacity <= 0:
        raise ValueError(f'{folder}: world cache was unavailable')
    for frame in frames:
        metadata = read_json(str(gates.frame_path(folder, frame)) + '.json')
        checks = dict(sceneFrames=frame, worldCacheCapacity=capacity, samplingPhase=args.phase,
                      width=3840, height=2160, framebufferWidth=3840, framebufferHeight=2160,
                      giWidth=3840, giHeight=2160, fullscreen=run['fullscreen'], backend='hardware',
                      gather='screen_probes', tierFallback='', asyncCompute=state['async_compute'])
        for field, expected_value in checks.items():
            if metadata.get(field) != expected_value:
                raise ValueError(f'{folder}, frame {frame}: expected {field}={expected_value}, got {metadata.get(field)}')
    return dict(command=command, actual=state, cache_cells=capacity, camera_sha256=digest(folder / 'camera-path.json'))


def replay(args):
    if not 0 < args.max_noise_pct <= 100:
        raise ValueError('The per-frame noise limit must be in (0, 100] percent')
    if args.executable.resolve() != DEFAULT_EXE.resolve():
        raise ValueError(f'Production replay uses the canonical {DEFAULT_EXE}')
    if args.smoke or args.references or args.set or args.hold < 64 or args.warmup < 2:
        raise ValueError('Production replay requires its own references, a fixed setup, warmup >= 2 and hold >= 64')
    if not args.rescore and running_renderers():
        raise RuntimeError('Another renderer is running')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=args.rescore)
    setup = read_json(args.setup)
    if setup['animationEnabled']:
        raise ValueError('Settled turn references require a static scene')
    setup['rendering']['gi']['cone']['samplingPhase'] = args.phase
    moving, rows, reference, ref_rows = schedules(setup, args.production_turn, args.degrees, args.warmup, args.hold)
    captures = [row['frame'] for row in rows]
    ref_frames = [f for samples in ref_rows.values() for f in samples['mean'] + samples['floor']]
    if not args.rescore:
        setup_path = args.output / 'moving/setup.zenrender'
        setup_path.parent.mkdir()
        write_json(setup_path, setup)
        identity = provenance(args, setup_path, [])
    runs = [capture(args, 'moving', setup, moving, captures),
            capture(args, 'reference', setup, reference, ref_frames)]
    if runs[0]['cache_cells'] != runs[1]['cache_cells']:
        raise ValueError(f"Moving and settled cache capacities differ: {[run['cache_cells'] for run in runs]}")
    if args.rescore:
        identity = read_json(args.output / 'identity.json')
        if identity['commands'] != [run['command'] for run in runs]:
            raise ValueError('Stored commands differ from the capture identity')
    else:
        end = provenance(args, setup_path, [run['command'] for run in runs])
        for field in ('inputs', 'setup_sha256', 'executable_sha256', 'shaders_sha256'):
            if identity[field] != end[field]:
                raise ValueError(f'Measurement input changed during replay: {field}')
        identity = end
        write_json(args.output / 'identity.json', identity)
    by_angle = defaultdict(list)
    for row in rows:
        by_angle[row['angle']].append(row)
    floor = {}
    for angle, samples in ref_rows.items():
        folder = args.output / 'reference'
        target_matrix = projection_view(folder, samples['mean'][0])
        if len(target_matrix) != 16 or any(projection_view(folder, f) != target_matrix for f in samples['mean'] + samples['floor']):
            raise ValueError(f'Reference camera moved during hold at {angle} degrees')
        mean = sum(load_image(folder, f).astype(np.float32) for f in samples['mean']) / len(samples['mean'])
        floor[angle] = [dict(frame=f, **settled_turn_metrics(load_image(folder, f), mean)) for f in samples['floor']]
        for row in by_angle[angle]:
            if projection_view(args.output / 'moving', row['frame']) != target_matrix:
                raise ValueError(f"Moving frame {row['frame']} has a different projection or view at {angle} degrees")
            row.update(settled_turn_metrics(load_image(args.output / 'moving', row['frame']), mean))
    mark_lifetimes(rows, args.output / 'moving')
    # Count large contiguous faults too, including interiors that have no contrast with their ring.
    worst = max(rows, key=lambda r: r['high_error_pct'])
    gate = dict(metric='high_error_pct', channel_difference_gt=24, limit_pct=args.max_noise_pct,
                worst_pct=worst['high_error_pct'], pixels=worst['high_error_pixels'], frame=worst['frame'],
                visit=worst['visit'], angle=worst['angle'], recovery=worst['recovery'],
                passed=worst['high_error_pct'] < args.max_noise_pct)
    write_json(args.output / 'report.json', dict(kind=args.production_turn, degrees=args.degrees, phase=args.phase,
                                                cache_cells=runs[0]['cache_cells'], runs=runs, references=ref_rows,
                                                moving=rows, floor=floor, noise_gate=gate))
    print(f"Worst high-error area {gate['worst_pct']:.5f}% at frame {gate['frame']} (limit {args.max_noise_pct:g}%)")
    return 0 if gate['passed'] else 1
