"""Matched glossy-view error against the retained sparse Monte Carlo reference.

Both candidates use the same one-secondary-surface lighting contract for this
reflection test. Multi-bounce diffuse accuracy is measured separately with CUDA.
"""
import argparse
import copy
from pathlib import Path

import numpy as np
from PIL import Image
from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json, zenapp
from gi_matched_quality import image, metrics, reference_mask
from gi_noise_probe import preset
from profile_hybrid_gi import running_renderers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--baseline', type=Path, help='Reuse a frozen reference/retained measurement with identical inputs')
    args = parser.parse_args()
    if running_renderers():
        parser.error('Close other Zen renderers before measuring')
    args.setup, args.output = args.setup.resolve(), args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    source = preset(read_json(args.setup), 'hardware')
    source['rendering']['gi']['cone'].update(indirectDiffuse=False, worldCacheFeedback=False,
                                            nativeShadowReuse=False, environmentLighting=False)
    source['rendering']['environment']['lighting'] = False
    source['run'].update(windowWidth=1280, windowHeight=720, fullscreen=False, followWindow=False)
    identity = provenance(args, args.setup, [])
    commands, pictures, states = [], {}, {}
    baseline = read_json(args.baseline/'report.json') if args.baseline else None
    if baseline:
        for key in ('inputs', 'setup_sha256'):
            if identity[key] != baseline['provenance'][key]:
                raise ValueError(f'Frozen baseline differs: {key}')
    for mode in ('reference', 'retained', 'native'):
        folder = args.output / mode
        if baseline and mode != 'native':
            folder = args.baseline / mode
            frames = [128, 256] if mode == 'reference' else [496, 512]
            pictures[mode] = [image(folder/f'capture.frame-{frame}.ppm') for frame in frames]
            states[mode] = baseline['states'][mode]
            continue
        folder.mkdir()
        setup = copy.deepcopy(source)
        cone = setup['rendering']['gi']['cone']
        cone.update(screenProbeGI=mode == 'native', worldRadianceCache=mode == 'native',
                    referenceSamples=64 if mode == 'reference' else 0,
                    referenceTileStride=8 if mode == 'reference' else 1)
        frames = [128, 256] if mode == 'reference' else [496, 512]
        path = folder / 'setup.zenrender'
        write_json(path, setup)
        command = zenapp(args, [f'--setup={path}', f'--frames={frames[-1]}', '--fixed-step', '--no-ui',
                               '--validation=1', '--async-compute=0', f'--capture={folder/"capture"}',
                               '--capture-frames=' + ','.join(map(str, frames))], folder / 'console.log')
        commands.append(command)
        log = (folder / 'console.log').read_text(errors='replace')
        for marker in ('[error]', 'VUID-', 'SYNC-HAZARD', 'Validation Error', 'Unknown or incompatible binding'):
            if marker in log:
                raise ValueError(f'{marker}: {folder}')
        captures = [folder/f'capture.frame-{frame}.ppm' for frame in frames]
        states[mode] = read_json(str(captures[-1]) + '.json')
        for key, value in dict(width=3840, height=2160, backend='hardware',
                                gather='screen_probes' if mode == 'native' else 'pixel',
                                referenceSamples=cone['referenceSamples']).items():
            if states[mode].get(key) != value:
                raise ValueError(f'{mode}: unexpected {key}={states[mode].get(key)!r}')
        pictures[mode] = [image(path) for path in captures]
        print(mode, flush=True)
    mask = reference_mask(3840, 2160)
    reference = pictures['reference'][-1]
    rows = {mode: metrics(values[-1], reference, mask) for mode, values in pictures.items() if mode != 'reference'}
    final = provenance(args, args.setup, commands)
    for key in ('executable_sha256', 'shaders_sha256', 'inputs', 'setup_sha256'):
        if identity[key] != final[key]:
            raise ValueError(f'Measurement input changed: {key}')
    report = dict(provenance=final, states=states, error=rows,
                  frozen_baseline=str(args.baseline.resolve()) if args.baseline else None,
                  frozen_baseline_report_sha256=digest(args.baseline/'report.json') if args.baseline else None,
                  reference_residual=metrics(pictures['reference'][0], reference, mask),
                  passed=rows['native']['rms_8bit'] <= rows['retained']['rms_8bit'])
    write_json(args.output/'report.json', report)
    Image.fromarray((mask*255).astype(np.uint8)).save(args.output/'reference-mask.png')
    print(rows, flush=True)


if __name__ == '__main__':
    main()
