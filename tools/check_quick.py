"""Quick tier, a few minutes: run while editing.

Incremental Release build of RenderCoreTest, the full RenderCoreTest suite (mock RHI, CPU only), clang-format on
changed C++ and GLSL files and the no-exceptions check. --native adds the Vulkan integration and ray-query suites on the GPU
(about two more minutes). Nothing here proves image output; use check_pr.py before submitting.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

from check_common import CHECKS, ROOT, build, changed_files, check_format, python, stamp, uses_installed_rhi, write_summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true', help='Also run VulkanRHIIntegrationTest and RayQueryIntegrationTest')
    parser.add_argument('--rhi-build-dir', type=Path, help='Include the standalone native ZenRHI suite')
    parser.add_argument('--filter', default='*', help='gtest filter for RenderCoreTest')
    args = parser.parse_args()
    output = CHECKS / f'quick-{stamp()}'
    build_dir = ROOT / 'build/x64-windows-msvc-release'
    suites = ['RayQueryIntegrationTest'] if args.native else []
    if args.native and not uses_installed_rhi(build_dir):
        suites.append('VulkanRHIIntegrationTest')
    targets = ['RenderCoreTest', *suites]
    build('release', targets, log=output / 'build.log')
    log = output / 'RenderCoreTest.log'
    with log.open('w') as stream:
        result = subprocess.run([str(ROOT / 'build/x64-windows-msvc-release/bin/RenderCoreTest.exe'), '--gtest_color=no',
                                 '--gtest_brief=1', f'--gtest_filter={args.filter}'], cwd=ROOT, stdout=stream,
                                stderr=subprocess.STDOUT)
    text = log.read_text(errors='replace')
    passed = re.search(r'\[  PASSED  \] (\d+) tests?', text)
    if result.returncode != 0 or 'MEMORY LEAK DETECTED' in text:
        print(text[-6000:])
        raise SystemExit(f'RenderCoreTest failed; see {log}')
    check_format(changed_files())
    python('tools/check_no_exceptions.py')
    native = {}
    if args.native:
        arguments = []
        if args.rhi_build_dir:
            suites.append('ZenRHIVulkanIntegrationTests')
            arguments.extend(['--rhi-build-dir', args.rhi_build_dir])
        python('tools/verify_rhi_production.py', '--build-dir', build_dir,
               '--output', output, *arguments, '--only', *suites)
        native = json.loads((output / 'results.json').read_text())
    write_summary(output, dict(tier='quick', render_core_tests=int(passed.group(1)) if passed else None,
                               native_tests=native, passed=True))


if __name__ == '__main__':
    main()
