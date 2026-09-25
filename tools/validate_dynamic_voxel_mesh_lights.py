"""Production GI light-mask regressions against analytically classified planes."""
import argparse
import json
from pathlib import Path

import numpy as np

import validate_dynamic_voxel_quality as quality
from voxelization_fixtures import Fixture, rgba_png


def fixture(folder, name):
    asset = Fixture()
    asset.document['materials'] = [dict(pbrMetallicRoughness=dict(
        baseColorFactor=[.7, .7, .7, 1], metallicFactor=0))]
    asset.mesh([[(-.5,)*3]*3, [(.5,)*3]*3])

    def panel(radius, z, reverse=False, material=0):
        points = [(-radius, -radius, z), (radius, -radius, z),
                  (radius, radius, z), (-radius, radius, z)]
        corners = ((2, 1, 0), (3, 2, 0)) if reverse else ((0, 1, 2), (0, 2, 3))
        uv = [(0, 0), (1, 0), (1, 1), (0, 1)]
        asset.mesh([[points[i] for i in t] for t in corners], material=material,
                   uv0=[uv[i] for t in corners for i in t])

    panel(.3, -.23)
    if name not in ('endpoint', 'clear'):
        material = 0
        if name == 'cutout':
            (folder/'transparent.png').write_bytes(rgba_png(2, 2, [(255, 255, 255, 0)]*4))
            asset.document.update(images=[dict(uri='transparent.png')], textures=[dict(source=0)])
            asset.document['materials'].append(dict(alphaMode='MASK', alphaCutoff=.5,
                pbrMetallicRoughness=dict(baseColorTexture=dict(index=0), metallicFactor=0)))
            material = 1
        panel(.15, -.07, name == 'backface', material)
    return asset.save(folder, name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    folder = out/'fixtures'; folder.mkdir(exist_ok=True)
    original = (quality.ROOT/'Data/engine.cfg').read_bytes()
    results = {}
    for producer in ('comp', 'geom'):
        for name in ('endpoint', 'clear', 'opaque', 'backface', 'cutout', 'beyond'):
            asset = fixture(folder, name)
            z = -.228 if name in ('endpoint', 'beyond') else .2
            tag = f'{producer}-{name}'
            prefix = quality.capture(out, tag, asset, 64, producer, False, original,
                overrides={'light_count': 1, 'light.0.position': f'0,0,{z}',
                           'dynamic_voxel_gi_emissive_lighting': 'false'})
            meta = json.loads(Path(str(prefix)+'.json').read_text())
            n = meta['resolution']; cells = n**3
            minimum = np.array(meta['grid_min']); size = meta['voxel_size']
            owners = np.fromfile(str(prefix)+'.voxels.bin', '<u4').reshape(n,n,n,8)[...,0]
            # Exported textures are XYZ-fastest; light masks use ZYX-fastest IDs.
            owned = owners.transpose(2,1,0).ravel()
            masks = np.fromfile(str(prefix)+'.static.bin', '<u4', count=cells, offset=192*cells)
            indices = np.arange(cells)
            centers = minimum + (np.stack((indices//(n*n), indices//n % n, indices % n), axis=1)+.5)*size
            selected = ((owned == 2) | (owned == 3)) & (np.max(np.abs(centers[:,:2]), axis=1) < .08)
            assert selected.sum() >= 64
            visible = (masks[selected] & 1) != 0
            expected = name not in ('opaque', 'backface')
            assert np.all(visible == expected), (tag, int(visible.sum()), int(selected.sum()))
            light_cell = np.floor((np.array([0,0,z])-minimum)/size).astype(int)
            endpoint_occupied = owners[light_cell[2],light_cell[1],light_cell[0]] != 0xffffffff
            if name in ('endpoint', 'beyond'):
                assert endpoint_occupied, 'Regression must put the light inside an occupied voxel'
            results[tag] = dict(cells=int(selected.sum()), visible=int(visible.sum()),
                expected_visible=expected, light_cell_occupied=bool(endpoint_occupied))
            print('PASS', tag, results[tag], flush=True)
    assert (quality.ROOT/'Data/engine.cfg').read_bytes() == original
    (out/'results.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
