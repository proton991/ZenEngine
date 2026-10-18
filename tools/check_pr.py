"""PR tier, about half an hour: run before submitting a change.

Debug and Release builds of every target; all 15 suites per build; the trimmed smoke matrix (8 cases per build plus
the 2-case previous-frame-radiance GI subset); then the 28 deterministic capture pairs against the last binary that
passed check_full.py. Byte-identical captures and an unchanged shader pack mean this change did not alter rendered
output in those configurations, so the full tier's image gates can wait for the next phase close. Any difference is
reported as requiring check_full.py; it is not a failure of this tier by itself.
"""
import argparse
import hashlib
import json
import subprocess

from check_common import (CHECKS, ROOT, SUITES, VENV_PYTHON, build, changed_files, check_format, python,
                          qualified_binary, stamp, verify_suites, write_summary)


def shader_hashes(folder):
    return {path.relative_to(folder).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(folder.rglob('*.spv'))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', help='Frozen binary folder to compare against (default: build/checks/qualified.json)')
    parser.add_argument('--skip-debug', action='store_true', help='Release only; Debug adds forced swapchain recreation')
    parser.add_argument('--skip-captures', action='store_true')
    args = parser.parse_args()
    output = CHECKS / f'pr-{stamp()}'
    check_format(changed_files())
    python('tools/check_no_exceptions.py')
    summary = dict(tier='pr', output=str(output), builds={})
    for configuration in (['release'] if args.skip_debug else ['debug', 'release']):
        build(configuration, log=output / configuration / 'build.log')
        python('build/run_production_fix_suites.py', configuration, '--output', output, '--only', *SUITES)
        tests = verify_suites(output / configuration, SUITES)
        python('build/run_production_fix_smoke.py', configuration, '--trimmed', '--output', output)
        python('build/verify_production_results.py', output / configuration, '--smoke', '--smoke-folder', 'smoke-trimmed',
               '--smoke-count', '8')
        python('build/run_delayed_radiance_smoke.py', configuration, '--trimmed', '--output', output / 'delayed-radiance')
        summary['builds'][configuration] = dict(tests=sum(tests.values()), suites=len(tests), smokes=8 + 2)
    if not args.skip_captures:
        baseline = qualified_binary(args.baseline)
        frozen = output / 'binary'
        python('build/freeze_production_binary.py', '--output', frozen, '--scope', 'PR tier candidate')
        command = [str(VENV_PYTHON), 'build/compare_production_captures.py',
                   '--baseline', str(baseline / 'scene_renderer_demo.exe'), '--baseline-shaders', str(baseline / 'SpvShaders'),
                   '--current', str(frozen / 'scene_renderer_demo.exe'), '--current-shaders', str(frozen / 'SpvShaders'),
                   '--assets', 'E:/Dev/glTF-Sample-Assets/Models', '--output', str(output / 'captures')]
        print('RUN', subprocess.list2cmdline(command), flush=True)
        process = subprocess.run(command, cwd=ROOT)
        results = json.loads((output / 'captures/results.json').read_text())
        identical = sum(1 for case in results['cases'] if case.get('identical'))
        same_shaders = shader_hashes(frozen / 'SpvShaders') == shader_hashes(baseline / 'SpvShaders')
        complete = len(results['cases']) == 28 and results.get('shaders_restored', False)
        assert complete or process.returncode == 0, 'Capture comparison did not finish; see captures/*/run.log'
        unchanged = complete and identical == 28 and same_shaders
        summary['images'] = dict(baseline=str(baseline), identical_pairs=identical, pairs=len(results['cases']),
                                 shader_pack_unchanged=same_shaders, rendered_output_unchanged=unchanged,
                                 full_tier_needed=not unchanged)
        print('Rendered output unchanged; full image gates not required for this change.' if unchanged else
              'Rendered output or shaders changed; run tools/check_full.py before closing the phase.', flush=True)
    summary['passed'] = True
    write_summary(output, summary)


if __name__ == '__main__':
    main()
