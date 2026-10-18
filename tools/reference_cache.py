"""Input-keyed cache for native captures that render converged references.

Enabled with ZEN_REFERENCE_CACHE=strict; unset or 'off' leaves every tool's behavior unchanged.

The key covers the executable, every SPIR-V module, the engine configuration written for the capture, the scene and
environment inputs, and the native command line, with this run's output folder replaced by a placeholder. Only those
inputs can change a capture, so a hit replays the outputs of an identical earlier run instead of re-rendering its
65,536-sample references. A changed binary or shader pack never hits; references are not reused across versions.

Captures whose references are rendered in-process after scripted events (stability, camera motion, light changes)
depend on that process's state, so they are cached and replayed as one unit with all of their outputs.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]
VERSION = 1
# Cached file names replace the capture prefix with this stem (Windows forbids '<' and '>').
STEM = 'capture'


def enabled():
    return os.environ.get('ZEN_REFERENCE_CACHE', 'off') == 'strict'


def cache_root():
    return Path(os.environ.get('ZEN_REFERENCE_CACHE_DIR', ROOT / 'build/reference-cache'))


def _sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def key(prefix, command, configuration, inputs, executable):
    """Digest of everything that determines a capture; paths under the output folder are normalized."""
    folder = str(Path(prefix).resolve().parent)
    variants = {folder, folder.replace('\\', '/')}

    def normalize(text):
        for variant in variants:
            text = text.replace(variant, '<output>')
        return text

    shaders = ROOT / 'Data/SpvShaders'
    material = dict(version=VERSION, executable=_sha256(executable),
                    shaders={path.relative_to(shaders).as_posix(): _sha256(path) for path in sorted(shaders.rglob('*.spv'))},
                    configuration=normalize(configuration.decode('utf-8', errors='replace')),
                    inputs=sorted(_sha256(path) for path in inputs),
                    command=[normalize(str(part)) for part in command[1:]])
    digest = hashlib.sha256(json.dumps(material, sort_keys=True).encode()).hexdigest()
    return digest, material


def clean_log(path):
    """Only clean captures are stored, so a transient validation error is never replayed."""
    text = Path(path).read_text(errors='replace')
    return re.search(r'\[error\]|VUID-|SYNC-HAZARD|\bhazard detected\b|MEMORY LEAK DETECTED', text) is None


def _outputs(prefix):
    prefix = Path(prefix)
    return sorted(path for path in prefix.parent.iterdir() if path.is_file()
                  and (path.name.startswith(prefix.name + '.') or path.name.startswith(prefix.name + '-'))
                  and not path.name.endswith('.inputs.json') and path.name != prefix.name + '.gltf')


def restore(digest, prefix):
    """Copy a cached capture to this prefix. False on a miss or an incomplete entry."""
    entry = cache_root() / digest
    manifest_path = entry / 'manifest.json'
    found = manifest_path.exists()
    if found:
        manifest = json.loads(manifest_path.read_text())
        prefix = Path(prefix)
        for name, expected in manifest['files'].items():
            source = entry / 'files' / name
            found = found and source.exists() and _sha256(source) == expected
        if found:
            for name in manifest['files']:
                shutil.copy2(entry / 'files' / name, prefix.parent / (prefix.name + name[len(STEM):]))
            print(f'Reference cache hit {digest[:16]}: {len(manifest["files"])} files for {prefix.name}', flush=True)
    return found


def store(digest, material, prefix):
    """Record a successful capture's outputs under its key; an existing entry is kept."""
    entry = cache_root() / digest
    if not (entry / 'manifest.json').exists():
        staging = cache_root() / f'.{digest}.{os.getpid()}'
        shutil.rmtree(staging, ignore_errors=True)
        (staging / 'files').mkdir(parents=True)
        files = {}
        prefix = Path(prefix)
        for path in _outputs(prefix):
            name = STEM + path.name[len(prefix.name):]
            shutil.copy2(path, staging / 'files' / name)
            files[name] = _sha256(path)
        (staging / 'manifest.json').write_text(json.dumps(dict(key=digest, material=material, files=files), indent=2))
        try:
            staging.rename(entry)
            print(f'Reference cache stored {digest[:16]}: {len(files)} files for {prefix.name}', flush=True)
        except OSError:
            shutil.rmtree(staging, ignore_errors=True)  # A concurrent writer stored the same key first.
