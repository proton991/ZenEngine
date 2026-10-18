"""Shared helpers for the quick/PR/full check tiers (tools/check_*.py)."""
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CHECKS = ROOT / 'build/checks'
VENV_PYTHON = ROOT / 'build/venv/Scripts/python.exe'
VS_DEV_CMD = Path('C:/Program Files/Microsoft Visual Studio/18/Professional/Common7/Tools/VsDevCmd.bat')
QUALIFIED = CHECKS / 'qualified.json'
SUITES = ['RenderCoreTest', 'VulkanRHITest', 'CommonTest', 'EditorModelTest', 'EditorInputTest', 'UIDrawPacketTest',
          'VulkanRHIIntegrationTest', 'RayQueryIntegrationTest', 'EditorRenderingTest', 'EditorPreviewTest',
          'UIRenderingTest', 'RuntimeUIIntegrationTest', 'UIPlatformTest', 'SceneModelSwitchTest',
          'ConeVoxelGIIntegrationTest']
FORMAT_ROOTS = ('ZenCore/', 'ZenSamples/', 'ZenUI/')  # tools/format_active.py scope
CPP_SUFFIXES = ('.h', '.hpp', '.cpp', '.inl')


def stamp():
    return datetime.datetime.now().strftime('%Y%m%d-%H%M%S')


def run(command, **options):
    command = [str(part) for part in command]
    print('RUN', subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True, **options)


def python(script, *arguments, venv=False):
    run([VENV_PYTHON if venv else sys.executable, script, *arguments])


def build(configuration, targets=(), log=None):
    """Incremental CMake build in a Visual Studio developer environment."""
    target_arguments = f" --target {' '.join(targets)}" if targets else ''
    script = (f'@echo off\ncall "{VS_DEV_CMD}" -arch=x64 -host_arch=x64 >nul\nif errorlevel 1 exit /b %errorlevel%\n'
              f'cmake --build build/x64-windows-msvc-{configuration}{target_arguments} -j 12\n')
    with tempfile.NamedTemporaryFile('w', suffix='.cmd', delete=False) as handle:
        handle.write(script)
    print('BUILD', configuration, *(targets or ['<all>']), flush=True)
    try:
        if log is None:
            subprocess.run(['cmd.exe', '/d', '/c', handle.name], cwd=ROOT, check=True)
        else:
            Path(log).parent.mkdir(parents=True, exist_ok=True)
            with Path(log).open('w') as stream:
                result = subprocess.run(['cmd.exe', '/d', '/c', handle.name], cwd=ROOT, stdout=stream,
                                        stderr=subprocess.STDOUT)
            if result.returncode != 0:
                print(Path(log).read_text(errors='replace')[-4000:])
                raise SystemExit(f'Build failed ({configuration}); see {log}')
    finally:
        os.unlink(handle.name)


def changed_files():
    """Files modified relative to HEAD plus untracked files, as repository-relative POSIX paths."""
    tracked = subprocess.check_output(['git', 'diff', '--name-only', 'HEAD'], cwd=ROOT, text=True).splitlines()
    untracked = subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], cwd=ROOT,
                                        text=True).splitlines()
    return sorted(name for name in set(tracked + untracked) if (ROOT / name).is_file())


def check_format(files):
    """clang-format check limited to changed files inside tools/format_active.py's scope."""
    selected = [name for name in files if name.endswith(CPP_SUFFIXES) and name.startswith(FORMAT_ROOTS)
                and name != 'ZenCore/Source/vk_mem_alloc.cpp']
    for start in range(0, len(selected), 40):
        run(['clang-format', '--style=file', '--fallback-style=none', '--dry-run', '--Werror',
             *selected[start:start + 40]])
    print(f'Format check passed for {len(selected)} changed C++ files', flush=True)


def verify_suites(folder, names):
    """Check build/run_production_fix_suites.py results for the selected suites."""
    rows = {row['name']: row for row in json.loads((Path(folder) / 'suites.json').read_text())}
    for name in names:
        row = rows[name]
        assert row['exit_code'] == 0 and row['hazards'] == 0 and not row.get('failures'), (name, row.get('failures'))
        assert int(row['gtest']['tests']) > 0 and int(row['gtest']['failures']) == 0, name
    print(f'Verified {len(names)} suites in {folder}', flush=True)
    return {name: int(rows[name]['gtest']['tests']) for name in names}


def qualified_binary(override=None):
    """Last binary that passed the full tier, used as the PR tier's image baseline."""
    if override:
        return Path(override).resolve()
    if QUALIFIED.exists():
        return Path(json.loads(QUALIFIED.read_text())['binary'])
    fallback = ROOT / 'build/production-fix-phase4/descriptor-minimal-final-binary'
    print(f'No {QUALIFIED.relative_to(ROOT)}; using {fallback.relative_to(ROOT)} as the image baseline', flush=True)
    return fallback


def write_summary(folder, summary):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2), flush=True)
