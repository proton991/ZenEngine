"""Original self-contained fixture for cooked geometry and animated render parity."""
import base64
import json
import math
from pathlib import Path
import struct


def generate(reverse=False):
    data = bytearray()
    views = []
    accessors = []

    def accessor(values, components, kind, component_type=5126, bounds=False):
        while len(data) % 4:
            data.append(0)
        flat = [value for row in values for value in row]
        encoding = 'f' if component_type == 5126 else 'H'
        payload = struct.pack('<' + encoding * len(flat), *flat)
        view = len(views)
        views.append({'buffer': 0, 'byteOffset': len(data), 'byteLength': len(payload)})
        data.extend(payload)
        result = {'bufferView': view, 'componentType': component_type,
                  'count': len(values), 'type': kind}
        if bounds:
            result['min'] = [min(row[i] for row in values) for i in range(components)]
            result['max'] = [max(row[i] for row in values) for i in range(components)]
        accessors.append(result)
        return len(accessors) - 1

    # Two coplanar overlapping triangles in one blended primitive. Equal depth
    # exercises the renderer's stable sort: changing source order then changes
    # compositing, while scrambling vertex storage must preserve that order.
    positions = [(-.8, -.7, 0), (.8, -.7, 0), (0, .8, 0),
                 (-.7, -.6, 0), (.7, -.6, 0), (0, .7, 0)]
    colors = [(1, .12, .06, .6)] * 3 + [(.06, .55, 1, .6)] * 3
    uvs = [(0, 0), (1, 0), (.5, 1)] * 2
    permutation = [2, 4, 0, 5, 1, 3]
    stored = [permutation.index(i) for i in range(6)]
    def reorder(values):
        return [values[i] for i in stored]

    weights = [(0.8, .2, 0, 0), (.8, .2, 0, 0), (0, 1, 0, 0)] * 2
    delta = [(-.25, .1, 0), (.2, -.1, 0), (.35, .25, 0)] * 2
    attributes = {
        'POSITION': accessor(reorder(positions), 3, 'VEC3', bounds=True),
        'NORMAL': accessor([(0, 0, 1)] * 6, 3, 'VEC3'),
        'COLOR_0': accessor(reorder(colors), 4, 'VEC4'),
        'TEXCOORD_0': accessor(reorder(uvs), 2, 'VEC2'),
        'TEXCOORD_1': accessor(reorder([(1-u, v) for u, v in uvs]), 2, 'VEC2'),
        'JOINTS_0': accessor([(0, 1, 0, 0)] * 6, 4, 'VEC4', 5123),
        'WEIGHTS_0': accessor(reorder(weights), 4, 'VEC4'),
        '_FREQUENCY': accessor(reorder([(float(i), -float(i)) for i in range(6)]), 2, 'VEC2'),
    }
    draw_order = permutation[3:] + permutation[:3] if reverse else permutation
    indices = accessor([(i,) for i in draw_order], 1, 'SCALAR', 5123)
    target = accessor(reorder(delta), 3, 'VEC3', bounds=True)
    identity = tuple(1.0 if i % 5 == 0 else 0.0 for i in range(16))
    bind = accessor([identity, identity], 16, 'MAT4')
    times = accessor([(0,), (1,), (2,)], 1, 'SCALAR', bounds=True)
    rotations = accessor([(0, 0, math.sin(angle/2), math.cos(angle/2))
                          for angle in (0, .65, -.35)], 4, 'VEC4')
    morph = accessor([(0,), (1,), (.25,)], 1, 'SCALAR')
    clips = []
    for name, skin, shape in [('Skin', True, False), ('Morph', False, True), ('Combined', True, True)]:
        samplers, channels = [], []
        for enabled, output, node, path in [(skin, rotations, 2, 'rotation'), (shape, morph, 0, 'weights')]:
            if enabled:
                channels.append({'sampler': len(samplers), 'target': {'node': node, 'path': path}})
                samplers.append({'input': times, 'output': output, 'interpolation': 'LINEAR'})
        clips.append({'name': name, 'samplers': samplers, 'channels': channels})
    document = {
        'asset': {'version': '2.0', 'generator': 'ZenEngine original geometry parity fixture'},
        'buffers': [{'byteLength': len(data), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(data).decode()}],
        'bufferViews': views, 'accessors': accessors,
        'materials': [{'name': 'Transparent layers', 'alphaMode': 'BLEND', 'doubleSided': True,
                       'pbrMetallicRoughness': {'metallicFactor': 0, 'roughnessFactor': .7}}],
        'meshes': [{'name': 'Shuffled skin and morph', 'weights': [0],
                    'primitives': [{'attributes': attributes, 'indices': indices, 'material': 0,
                                    'targets': [{'POSITION': target}]}]}],
        'nodes': [{'name': 'Animated mesh', 'mesh': 0, 'skin': 0},
                  {'name': 'Root joint', 'children': [2]}, {'name': 'Tip joint'}],
        'skins': [{'joints': [1, 2], 'skeleton': 1, 'inverseBindMatrices': bind}],
        'animations': clips, 'scenes': [{'nodes': [0, 1]}], 'scene': 0,
    }
    filename = 'geometry_scene_reversed.gltf' if reverse else 'geometry_scene.gltf'
    Path(__file__).with_name(filename).write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    generate()
    generate(reverse=True)
