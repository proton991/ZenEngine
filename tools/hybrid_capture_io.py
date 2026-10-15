"""Lossless, bounded storage for the large diagnostic capture buffers."""
import gzip
import hashlib
from pathlib import Path


def read_bytes(path):
    path = Path(path)
    if path.exists():
        return path.read_bytes()
    with gzip.open(str(path) + '.gz', 'rb') as stream:
        return stream.read()


def compact(prefix):
    """Replace only this capture's three generated buffers after verifying the archive."""
    report = {}
    for suffix in ('.hybrid.bin', '.lighting.bin', '.environment.bin'):
        path = Path(str(prefix) + suffix)
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        archive = Path(str(path) + '.gz')
        with gzip.open(archive, 'wb', compresslevel=1) as stream:
            stream.write(data)
        with gzip.open(archive, 'rb') as stream:
            if hashlib.sha256(stream.read()).hexdigest() != digest:
                raise ValueError(f'Capture archive verification failed: {archive}')
        report[suffix] = dict(sha256=digest, bytes=len(data), archive_bytes=archive.stat().st_size)
        path.unlink()
    return report
