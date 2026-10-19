"""Quick tier, a few minutes: run while editing.

Incremental Release build of RenderCoreTest, the full RenderCoreTest suite (mock RHI, CPU only), clang-format on
changed C++ and GLSL files and the no-exceptions check. --native adds the Vulkan integration and ray-query suites on the GPU
(about two more minutes). Nothing here proves image output; use check_pr.py before submitting.
"""
import argparse
import re
import subprocess

from check_common import CHECKS, ROOT, build, changed_files, check_format, python, stamp, verify_suites, write_summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true', help='Also run VulkanRHIIntegrationTest and RayQueryIntegrationTest')
    parser.add_argument('--filter', default='*', help='gtest filter for RenderCoreTest')
    args = parser.parse_args()
    output = CHECKS / f'quick-{stamp()}'
    targets = ['RenderCoreTest'] + (['VulkanRHIIntegrationTest', 'RayQueryIntegrationTest'] if args.native else [])
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
        python('build/run_production_fix_suites.py', 'release', '--output', output, '--only', 'VulkanRHIIntegrationTest',
               'RayQueryIntegrationTest')
        native = verify_suites(output / 'release', ['VulkanRHIIntegrationTest', 'RayQueryIntegrationTest'])
    write_summary(output, dict(tier='quick', render_core_tests=int(passed.group(1)) if passed else None,
                               native_tests=native, passed=True))


if __name__ == '__main__':
    main()
