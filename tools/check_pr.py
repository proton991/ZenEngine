"""Build and run registered Debug/Release tests before submitting a change.

Checks formatting and the no-exceptions policy, then uses CTest from each current
build. Native suites require a desktop and Vulkan validation. For an application
smoke matrix, run verify_rhi_production.py --smoke --setup=<file.zenrender>.
Image comparisons against historical demo binaries are not part of this check.
"""
import argparse
import subprocess

from check_common import CHECKS, ROOT, build, changed_files, check_format, python, rtss_running, stamp, write_summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-debug', action='store_true', help='Build and test Release only')
    args = parser.parse_args()
    if rtss_running():
        print('RTSS is running: it injects into native test presents and can crash Vulkan suites. Close it for a reliable '
              'result.', flush=True)
    output = CHECKS / f'pr-{stamp()}'
    check_format(changed_files())
    python('tools/check_no_exceptions.py')
    summary = dict(tier='pr', output=str(output), builds={})
    for configuration in (['release'] if args.skip_debug else ['debug', 'release']):
        folder = output / configuration
        build(configuration, log=folder / 'build.log')
        command = ['ctest', '--test-dir', f'build/x64-windows-msvc-{configuration}', '--output-on-failure']
        with (folder / 'tests.log').open('w') as stream:
            process = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
        summary['builds'][configuration] = dict(command=command, exit_code=process.returncode)
        summary['passed'] = process.returncode == 0
        write_summary(output, summary)
        if process.returncode:
            raise SystemExit(f'CTest failed; see {folder / "tests.log"}')


if __name__ == '__main__':
    main()
