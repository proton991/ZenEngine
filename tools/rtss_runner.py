"""Run RTSS-excluded binaries at one path so Windows can reuse its firewall approval.

Keep the lease for the entire child process lifetime. The OS releases the lock if a
runner crashes; another caller fails before replacing an executable still in use.
The executable stays at bin/7zFM.exe between runs. This helper does not change any
firewall or RTSS settings.

Use ``with rtss_executable(source) as executable:`` around subprocess execution;
store logs and captures in the run's output folder, not the executable itself.
"""

from contextlib import contextmanager
import os
from pathlib import Path
import shutil

if os.name == 'nt':
    import msvcrt
else:
    import fcntl


RTSS_RUNNER = Path(__file__).resolve().parents[1] / 'bin' / '7zFM.exe'


@contextmanager
def rtss_executable(source):
    source = Path(source).resolve(strict=True)
    if not source.is_file():
        raise ValueError(f'Not an executable file: {source}')
    RTSS_RUNNER.parent.mkdir(parents=True, exist_ok=True)
    with RTSS_RUNNER.with_suffix('.lock').open('a+b') as lock:
        lock.seek(0)
        try:
            if os.name == 'nt':
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError(f'{RTSS_RUNNER} is in use; run profiling/tests serially') from error
        try:
            if source != RTSS_RUNNER.resolve():
                shutil.copy2(source, RTSS_RUNNER)
                for library in source.parent.glob('*.dll'):
                    destination = RTSS_RUNNER.parent / library.name
                    if library.resolve() != destination.resolve():
                        shutil.copy2(library, destination)
            yield RTSS_RUNNER
        finally:
            lock.seek(0)
            if os.name == 'nt':
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
