"""Measure static, rigid and morph updates of one primitive in frozen Sponza.

The matched static copy has the same mesh split. Geometry, materials and camera
are retained; the selected primitive moves by 20 source-space units, or bends
by that amplitude. These are production-scene update costs, not image-quality
references. The scene's existing animation playback drives fixed 1/60 steps.
"""
import argparse
import copy
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
from urllib.parse import urlparse
from urllib.request import url2pathname

from capture_hybrid_gi import CAMERAS, ROOT, fingerprint


def prepare(source, output, primitive_index):
    document = json.loads(source.read_text())
    assert len(document['meshes']) == 1 and len(document['scenes']) == 1
    assert document['nodes'][-1].get('matrix') == CAMERAS['hall'], 'Expected the frozen hall camera'
    manifest = json.loads(source.with_suffix('.inputs.json').read_text())
    for entry in manifest['source_inputs']:
        if fingerprint(Path(entry['path']))['sha256'] != entry['sha256']:
            raise ValueError(f'Stale frozen-scene dependency: {entry["path"]}')
    primitive = document['meshes'][0]['primitives'].pop(primitive_index)
    positions = document['accessors'][primitive['attributes']['POSITION']]
    assert positions['type'] == 'VEC3' and positions['componentType'] == 5126 and 'sparse' not in positions
    view = document['bufferViews'][positions['bufferView']]
    uri = document['buffers'][view['buffer']]['uri']
    path = Path(url2pathname(urlparse(uri).path)) if uri.startswith('file:') else source.parent / uri
    binary = path.read_bytes()
    offset = view.get('byteOffset', 0) + positions.get('byteOffset', 0)
    stride = view.get('byteStride', 12)
    vertices = [struct.unpack_from('<3f', binary, offset + i * stride) for i in range(positions['count'])]
    low, high = min(p[1] for p in vertices), max(p[1] for p in vertices)
    assert high > low
    document['meshes'].append({'name': 'P8 updated primitive', 'primitives': [primitive]})
    node = copy.deepcopy(document['nodes'][0])
    assert node['mesh'] == 0 and 'matrix' not in node and 'children' not in node
    node['mesh'] = 1
    index = len(document['nodes'])
    document['nodes'].append(node)
    document['scenes'][0]['nodes'].append(index)
    scenes = {}
    for mode in ('static', 'rigid', 'deform'):
        data = copy.deepcopy(document)
        blob = bytearray()
        buffer_index = len(data['buffers'])

        def accessor(values, kind, count):
            flat = [v for value in values for v in value]
            payload = struct.pack('<' + 'f' * len(flat), *flat)
            data['bufferViews'].append(dict(buffer=buffer_index, byteOffset=len(blob), byteLength=len(payload)))
            data['accessors'].append(dict(bufferView=len(data['bufferViews'])-1, componentType=5126,
                type=kind, count=count, min=[min(v[i] for v in values) for i in range(len(values[0]))],
                max=[max(v[i] for v in values) for i in range(len(values[0]))]))
            blob.extend(payload)
            return len(data['accessors']) - 1

        if mode != 'static':
            times = accessor([(0,), (1,), (2,), (3,), (4,)], 'SCALAR', 5)
            if mode == 'rigid':
                assert 'translation' not in node and 'rotation' not in node
                # glTF node translation is applied after its scale. Match the
                # morph's 20 mesh-space units, rather than moving 20 parent units.
                amplitude = 20 * node.get('scale', [1, 1, 1])[0]
                values = accessor([(0, 0, 0), (amplitude, 0, 0), (0, 0, 0),
                                   (-amplitude, 0, 0), (0, 0, 0)], 'VEC3', 5)
                target = 'translation'
            else:
                delta = [(20 * math.sin(math.pi*(p[1]-low)/(high-low)), 0, 0) for p in vertices]
                position_delta = accessor(delta, 'VEC3', len(vertices))
                data['meshes'][1]['primitives'][0]['targets'] = [{'POSITION': position_delta}]
                data['meshes'][1]['weights'] = [0]
                values = accessor([(0,), (1,), (0,), (-1,), (0,)], 'SCALAR', 5)
                target = 'weights'
            data['animations'] = [dict(name='P8 '+mode,
                samplers=[dict(input=times, output=values, interpolation='LINEAR')],
                channels=[dict(sampler=0, target=dict(node=index, path=target))])]
            buffer_path = output / (mode+'.bin')
            buffer_path.write_bytes(blob)
            data['buffers'].append(dict(uri=buffer_path.name, byteLength=len(blob)))
        scene_path = output / (mode+'.gltf')
        scene_path.write_text(json.dumps(data))
        scenes[mode] = scene_path.resolve()
    return scenes, dict(source=fingerprint(source), source_inputs=manifest['source_inputs'],
                        tool=fingerprint(Path(__file__)), primitive_index=primitive_index,
                        updated_vertices=len(vertices), amplitude_source_units=20)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True, help='Frozen captured hall glTF')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--primitive', type=int, default=12)
    parser.add_argument('--frames', type=int, default=1200)
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    scenes, report = prepare(args.scene.resolve(), args.output.resolve(), args.primitive)
    report['cases'] = {}
    report['complete'] = False
    config = ROOT / 'Data/engine.cfg'
    original = config.read_bytes()
    if not args.prepare_only:
        for backend in ('compute', 'hardware'):
            for mode, scene in scenes.items():
                name = mode+'-'+backend
                command = [sys.executable, str(ROOT/'tools/benchmark_voxel_gi.py'), '--executable', str(args.exe.resolve()),
                    '--output', str((args.output/name).resolve()), '--backend', backend, '--profile', '--frames', str(args.frames)]
                settings = dict(default_model_path=scene.as_posix(), voxel_resolution=64,
                    scene_lighting_override='true', light_count=0, environment_texture='papermill.ktx',
                    environment_lighting='true', environment_intensity=1, environment_rotation_degrees=0,
                    voxel_gi_samples=4, voxel_gi_reference_samples=0, voxel_gi_adaptive_sampling='true',
                    voxel_gi_history_frames=32, voxel_gi_temporal='true', voxel_gi_filter='true',
                    voxel_gi_indirect_intensity=1, **{'dynamic_light.enabled': 'false', 'light_markers.enabled': 'false'})
                for key, value in settings.items():
                    command += ['--set', f'{key}={value}']
                with (args.output/(name+'.log')).open('w') as log:
                    process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
                if config.read_bytes() != original:
                    raise RuntimeError('Configuration changed externally; preserving the changes')
                result = json.loads((args.output/name/'summary.json').read_text()) if process.returncode == 0 else None
                report['cases'][name] = dict(summary=result, exit_code=process.returncode)
                (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
                if result is not None:
                    print(name, result['fps'], result['gpu_frame_ms']['measured'], flush=True)
                else:
                    print(name, f'FAILED ({process.returncode}); see {name}.log', flush=True)
        report['complete'] = True
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    if any(case['exit_code'] != 0 for case in report['cases'].values()):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
