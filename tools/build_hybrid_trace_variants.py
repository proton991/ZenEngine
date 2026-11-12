"""Compile isolated HybridTrace measurement modules without modifying Data/SpvShaders.

Pass the exact resulting .spv (or .omm.spv on an OMM device) to ZenApp or
profile_hybrid_gi.py with --hybrid-trace-module. Ablations change the image and
are measurement-only; they must never be used for quality qualification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VARIANTS = {
    'control': None,
    'combined': 'HYBRID_TRACE_COMBINED',
    'no-shadow': 'HYBRID_ABLATE_SHADOW',
    'constant-material': 'HYBRID_ABLATE_MATERIAL',
    'no-normal-map': 'HYBRID_ABLATE_NORMAL',
    'one-light': 'HYBRID_ABLATE_LIGHTS',
    'no-reflection': 'HYBRID_ABLATE_REFLECTION',
    'no-guide': 'HYBRID_ABLATE_GUIDE',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--variant', choices=VARIANTS, action='append')
    parser.add_argument('--buffered-guide', action='store_true',
                        help='Build 8x8 control/combined modules for --hybrid-trace-buffered-module')
    parser.add_argument('--no-environment', action='store_true',
                        help='Compile out guide and environment work; use only with environment lighting off')
    parser.add_argument('--hit-stream', action='store_true',
                        help='Compile compact hit-buffer reads; launch ZenApp with --hybrid-hit-stream')
    parser.add_argument('--define', action='append', default=[], metavar='NAME[=VALUE]',
                        help='Extra preprocessor definition for every module (experiments only)')
    parser.add_argument('--shader-root', type=Path, default=ROOT / 'Data/Shaders',
                        help='Shader tree to compile, for example an experiment copy of Data/Shaders')
    parser.add_argument('--entry', default='VoxelGI/hybrid_trace_restir_hardware.comp',
                        help='Shader entry file under the shader root, for ZenApp --hybrid-trace-override modules')
    parser.add_argument('--sdk', type=Path, default=Path(os.environ.get('VULKAN_SDK', '')))
    args = parser.parse_args()
    if args.no_environment and not args.buffered_guide:
        parser.error('--no-environment requires --buffered-guide')
    ablations = [name for name in VARIANTS if name not in ('control', 'combined')]
    default = (['control', 'combined'] + [name for name in ablations if name != 'no-guide'] if args.no_environment
               else ['control', 'combined'] if args.buffered_guide else list(VARIANTS))
    variants = args.variant or default
    if args.buffered_guide and not args.no_environment and any(name in ablations for name in variants):
        parser.error('Buffered environment modules support control and combined; ablate with --no-environment')
    if args.no_environment and 'no-guide' in variants:
        parser.error('--no-environment already compiles out the guide; no-guide is not a separate ablation there')
    shaders = args.shader_root.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    compiler = args.sdk / 'Bin/glslangValidator.exe'
    validator = args.sdk / 'Bin/spirv-val.exe'
    manifest = {'modules': [], 'sources': {}}
    manifest['shader_root'] = str(shaders)
    manifest['defines'] = args.define
    for folder in (shaders, ROOT / 'ZenCore/Include/Graphics/Shared'):
        for path in sorted(folder.rglob('*')):
            if path.is_file():
                manifest['sources'][path.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    for name in variants:
        for omm in (False, True):
            destination = output / (name + ('.omm' if omm else '') + '.spv')
            command = [str(compiler), '--target-env', 'spirv1.4', '-V',
                       f'-I{ROOT / "ZenCore/Include"}']
            if VARIANTS[name]:
                command.append(f'-D{VARIANTS[name]}=1')
            if args.buffered_guide:
                command.append('-DHYBRID_BUFFER_GUIDE=1')
            if args.no_environment:
                command.append('-DHYBRID_NO_GUIDE=1')
            if args.hit_stream:
                command.append('-DHYBRID_HIT_STREAM=1')
            if omm:
                command.append('-DZEN_OPACITY_MICROMAP=1')
            command += [f'-D{definition}' for definition in args.define]
            command += [str(shaders / args.entry),
                        '-o', str(destination)]
            subprocess.run(command, check=True)
            subprocess.run([str(validator), '--target-env', 'vulkan1.2', str(destination)], check=True)
            manifest['modules'].append(dict(path=str(destination), command=command,
                                             sha256=hashlib.sha256(destination.read_bytes()).hexdigest()))
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
