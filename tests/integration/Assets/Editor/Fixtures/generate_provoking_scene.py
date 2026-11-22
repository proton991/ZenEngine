"""Generate original flat-shading fixtures with shuffled vertex storage."""
from pathlib import Path
import base64
import json
import struct

directory = Path(__file__).resolve().parent
count = 8 * 8 * 3
positions = [None] * count
colors = [None] * count
indices = []
for triangle in range(64):
    x, y = triangle % 8, triangle // 8
    left, bottom = -0.9375 + x * 0.234375, -0.9375 + y * 0.234375
    corners = [(left, bottom), (left + 0.1875, bottom), (left, bottom + 0.1875)]
    for corner, (px, py) in enumerate(corners):
        logical = triangle * 3 + corner
        physical = (logical * 73 + 11) % count
        positions[physical] = (px, py, 0.5)
        rgb = [0.0, 0.0, 0.0, 1.0]
        rgb[(triangle + corner) % 3] = 1.0
        colors[physical] = rgb
        indices.append(physical)

for rotated in (False, True):
    order = list(indices)
    if rotated:
        for first in range(0, count, 3):
            order[first:first+3] = order[first+1:first+3] + order[first:first+1]
    payloads = [b''.join(struct.pack('<3f', *p) for p in positions),
                b''.join(struct.pack('<4f', *c) for c in colors),
                struct.pack('<' + 'I' * count, *order),
                struct.pack('<3f', 0.0, 0.0, 1.0) * count]
    blob = b''.join(payloads)
    offsets = [sum(map(len, payloads[:index])) for index in range(len(payloads))]
    document = {
        'asset': {'version': '2.0', 'generator': 'ZenAssetProcessor original provoking-vertex fixture'},
        'buffers': [{'byteLength': len(blob), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(blob).decode()}],
        'bufferViews': [{'buffer': 0, 'byteOffset': offset, 'byteLength': len(payload)} for offset, payload in zip(offsets, payloads)],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': count, 'type': 'VEC3',
             'min': [min(p[c] for p in positions) for c in range(3)],
             'max': [max(p[c] for p in positions) for c in range(3)]},
            {'bufferView': 1, 'componentType': 5126, 'count': count, 'type': 'VEC4'},
            {'bufferView': 2, 'componentType': 5125, 'count': count, 'type': 'SCALAR'},
            {'bufferView': 3, 'componentType': 5126, 'count': count, 'type': 'VEC3'}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'COLOR_0': 1, 'NORMAL': 3}, 'indices': 2, 'mode': 4}]}],
        'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
    name = 'provoking_rotated.gltf' if rotated else 'provoking_scene.gltf'
    (directory / name).write_text(json.dumps(document, separators=(',', ':')) + '\n')
