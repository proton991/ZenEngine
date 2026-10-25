"""Full tier, several hours: run to close a phase or after a change that alters rendered output.

Serially runs the complete production qualification on the current workspace: full Debug/Release builds, 15 suites
and both 24-case smoke matrices per build; freeze; 28 deterministic capture pairs against the last qualified binary;
32 strict resource profiles; benchmarks; static, dynamic and adaptive-light image gates;
720p/1080p timing; and the input/test-record audits. Each run writes new evidence folders under --name.

Converged references are cached by exact inputs (tools/reference_cache.py), so rerunning this tier on the same
frozen build, for example after an interrupted or failed late stage, replays them instead of re-rendering. On success
the frozen binary becomes the PR tier's image baseline (build/checks/qualified.json).
"""
import argparse
import json
import os

from check_common import CHECKS, QUALIFIED, ROOT, python, qualified_binary, stamp

PHASE4 = ROOT / 'build/production-fix-phase4'
PHASE8 = ROOT / 'build/production-fix-phase8'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', default=f'full-{stamp()}', help='Evidence prefix; folders must not exist yet')
    parser.add_argument('--baseline', help='Frozen binary folder for the 28 capture pairs (default: last qualified)')
    parser.add_argument('--no-reference-cache', action='store_true')
    args = parser.parse_args()
    if not args.no_reference_cache:
        os.environ['ZEN_REFERENCE_CACHE'] = 'strict'
    frozen = PHASE4 / f'{args.name}-binary'
    gate = PHASE8 / f'{args.name}-gate'
    baseline = qualified_binary(args.baseline)

    def step(script, *arguments):
        python(script, *arguments, venv=True)

    step('build/validate_descriptor_layout.py', '--output', PHASE4 / f'{args.name}-validation')
    step('build/qualify_descriptor_layout.py', '--prefix', args.name, '--baseline', baseline)
    gate.mkdir(parents=True, exist_ok=False)
    step('build/start_final_production_image_gate.py', '--prefix', args.name, '--gate', gate)
    step('build/qualify_production_dynamic_images.py', '--base', gate, '--frozen', frozen)
    step('build/validate_production_adaptive_lights.py', '--exe', gate / 'dynamic/7zFM.exe', '--base', gate)
    step('build/run_final_production_timing.py', '--base', gate, '--frozen', frozen)
    step('build/audit_final_production_inputs.py', '--frozen', frozen, '--output', gate / 'input-audit.json')
    step('build/audit_production_final_records.py', '--validation', PHASE4 / f'{args.name}-validation', '--output',
         gate / 'test-record-audit.json')
    CHECKS.mkdir(parents=True, exist_ok=True)
    QUALIFIED.write_text(json.dumps(dict(binary=str(frozen), gate=str(gate), name=args.name), indent=2) + '\n')
    print(f'FULL TIER PASSED; {frozen} is now the PR baseline. The manual monitor check remains separate.', flush=True)


if __name__ == '__main__':
    main()
