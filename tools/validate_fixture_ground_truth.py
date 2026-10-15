"""Compare Compute and HW RT one-bounce and specular fixtures with the CUDA references only.

Run with build/venv/Scripts/python.exe; use --capture once per build, then re-analyze saved data.

Captures (sequential; engine.cfg is restored by the runner) finish before any CUDA work starts.
Bounce: point-light room and thin wall at 128^3, uniform 0.5 albedo, no environment, against
tools/ground_truth_bounce.py (P0 limit: |bias| <= 10% on the converged reference).
Specular: glossy floor S against tools/ground_truth_specular.py (P0 limit: max |S error| <= 2%
absolute on the converged reference) and reflected emission C (report-only; tool flags 0.5% bias).
Shipping captures (64 frames) are compared with the same references and reported.
"""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate_hybrid_gi import run  # noqa: E402

BOUNCE = ('point_light_room', 'thin_wall_light')
KINDS = (('reference', 1024), ('shipping', 0))


def capture(exe, root, provider):
    common = dict(provider=provider, rt=provider == 'hardware', width=960, height=540, allow_present_baseline=True,
                  indirect=1, resolution=128, shadows=True)
    for kind, samples in KINDS:
        run(exe, root / 'bounce' / kind, list(BOUNCE), reference_samples=samples, bounce_source='rays',
            environment_lighting=False, **common)
        run(exe, root / 'specular' / kind, ['glossy_floor'], reference_samples=samples, **common)
        run(exe, root / 'emissive' / kind, ['glossy_emissive'], reference_samples=samples, environment_lighting=False,
            **common)


def analyze(root):
    from ground_truth_mitsuba import setup_mitsuba
    import ground_truth_bounce as bounce_truth
    import ground_truth_specular as specular_truth
    mi = setup_mitsuba('cuda')
    bounce_gate, specular_gate = bounce_truth.self_test(mi), specular_truth.self_test(mi)
    report = dict(variant=mi.variant(), bounce_fixture_gate=bounce_gate, specular_fixture_gate=specular_gate)
    for kind, _ in KINDS:
        for name in BOUNCE:
            prefix = root / 'bounce' / kind / name
            out = root / 'cuda' / 'bounce' / kind / name
            result = (json.loads(Path(str(out) + '.json').read_text()) if Path(str(out) + '.json').exists() else
                      bounce_truth.validate_capture(mi, prefix, out, scene_path=prefix.parent / 'fixtures' / (name + '.gltf'),
                                                    fixture_gate=bounce_gate))
            report[f'bounce/{kind}/{name}'] = dict(
                primary_mismatch=result['primary_mismatch'], bias_gate_passed=result['bias_gate_passed'],
                regions={k: {m: v[m] for m in ('pixels', 'bias', 'rms', 'excess_rms') if m in v}
                         for k, v in result['regions'].items()})
        for folder, name, emissive in (('specular', 'glossy_floor', False), ('emissive', 'glossy_emissive', True)):
            prefix = root / folder / kind / name
            out = root / 'cuda' / folder / kind / name
            result = (json.loads(Path(str(out) + '.json').read_text()) if Path(str(out) + '.json').exists() else
                      specular_truth.compare_capture(mi, prefix, prefix.parent / 'fixtures' / (name + '.gltf'), out,
                                                     samples=16384 if emissive else 65536, emissive=emissive,
                                                     fixture_gate=specular_gate))
            report[f'{folder}/{kind}/{name}'] = dict(receivers=result['receivers'], S=result['S'],
                                                     C=result.get('C'), truth_hit_fraction=result['truth_hit_fraction_mean'])
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--provider', choices=('voxel', 'hardware'), required=True)
    parser.add_argument('--capture', action='store_true')
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if args.capture:
        capture(args.exe.resolve(), root, args.provider)
    report = analyze(root)
    report['provider'] = args.provider
    (root / 'cuda-fixture-results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)
