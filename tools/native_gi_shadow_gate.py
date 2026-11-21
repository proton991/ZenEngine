"""Compare native shadow reconstruction with exact visibility at matched static and moving poses.

Indirect lighting is disabled in both captures, isolating direct-shadow changes. This
retains display-space errors and difference images; timing uses profile_hybrid_gi.py.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image
from gi_stability import DEFAULT_EXE, provenance, read_json, write_json
from gi_matched_quality import image, metrics
from gi_noise_probe import preset
import gi_quick_gates as gates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--executable', default=DEFAULT_EXE, type=Path)
    args = parser.parse_args()
    if gates.running_renderers():
        parser.error('Close other Zen renderers before measuring')
    args.setup, args.output = args.setup.resolve(), args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    args.validation = True
    setup = preset(read_json(args.setup), 'hardware')
    setup['rendering']['gi']['cone'].update(screenProbeGI=True, worldRadianceCache=True,
        indirectIntensity=0, environmentLighting=False, specularOcclusion=False, reflections=1)
    setup['rendering']['environment']['lighting'] = False
    frames = [32, 40, 64, 96, 104, 128, 160]
    keys = [gates.key(1, gates.base_pose(setup)), gates.key(32, gates.base_pose(setup))]
    keys += [gates.key(32+i, gates.motion_pose(setup, 'pan-fast', i)) for i in range(1,65)]
    keys += [gates.key(97, gates.base_pose(setup))]
    keys += [gates.key(97+i, gates.motion_pose(setup, 'dolly', i)) for i in range(1,64)]
    identity = provenance(args, args.setup, [])
    commands = []
    for mode in ('exact', 'native'):
        setup['rendering']['gi']['cone']['nativeShadowReuse'] = mode == 'native'
        commands.append(gates.launch(args, args.output/mode, setup, dict(keys=keys), None, frames, 160))
    rows = []
    for frame in frames:
        exact = image(gates.frame_path(args.output/'exact',frame))
        native = image(gates.frame_path(args.output/'native',frame))
        rows.append(dict(frame=frame, **metrics(native, exact)))
        difference = np.clip(np.abs(native-exact)*16,0,255).astype(np.uint8)
        Image.fromarray(difference).save(args.output/f'difference-{frame}.png')
    final = provenance(args,args.setup,commands)
    for key in ('executable_sha256','shaders_sha256','inputs','setup_sha256'):
        if final[key] != identity[key]:
            raise ValueError(f'Measurement input changed: {key}')
    write_json(args.output/'report.json',dict(provenance=final,frames=rows,difference_gain=16))
    print(rows,flush=True)


if __name__ == '__main__':
    main()
