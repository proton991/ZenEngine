"""Build an isolated native-pixel reference module and its stratified sampling mask.

One complete 8x8 workgroup is selected uniformly within each 64x64-pixel stratum.
The reference's unfiltered pointwise integration is unchanged at selected pixels.
Validate selected pixels against a full dispatch before using this module for metrics.
No shipped shader is replaced. Environment lighting must be disabled.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

import numpy as np
from PIL import Image

from gi_stability import ROOT, digest, write_json


def hash32(value):
    value ^= value >> 16
    value = (value * 0x7feb352d) & 0xffffffff
    value ^= value >> 15
    value = (value * 0x846ca68b) & 0xffffffff
    return value ^ (value >> 16)


def main(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    shutil.copytree(ROOT / 'Data/Shaders', output / 'Shaders')
    shutil.copytree(ROOT / 'ZenCore/Include/Graphics/Shared', output / 'Include/Graphics/Shared')
    trace = output / 'Shaders/VoxelGI/hybrid_trace.glsl'
    source = trace.read_text()
    location = source.rfind('    if (IsReceiver(p))')
    if location < 0:
        raise ValueError('Reference trace entry point not found')
    replacement = '''    uvec2 stratum = gl_WorkGroupID.xy / 8u;
    uvec2 available = min(uvec2(8u), gl_NumWorkGroups.xy - stratum * 8u);
    uint chosen = HybridHash(stratum.x + 4099u * stratum.y + 0x3c6ef372u) % (available.x * available.y);
    uvec2 selected = uvec2(chosen % available.x, chosen / available.x);
    bool measured = all(equal(gl_WorkGroupID.xy % 8u, selected));
    if (IsReceiver(p) && measured)'''
    trace.write_text(source[:location] + source[location:].replace('    if (IsReceiver(p))', replacement, 1))
    mask = np.zeros((2160, 3840), np.uint8)
    for y in range(0, 270, 8):
        for x in range(0, 480, 8):
            nx, ny = min(8, 480 - x), min(8, 270 - y)
            chosen = hash32(x // 8 + 4099 * (y // 8) + 0x3c6ef372) % (nx * ny)
            px, py = (x + chosen % nx) * 8, (y + chosen // nx) * 8
            mask[py:py + 8, px:px + 8] = 255
    Image.fromarray(mask).save(output / 'mask.png')
    compiler, validator = args.sdk / 'Bin/glslangValidator.exe', args.sdk / 'Bin/spirv-val.exe'
    modules = []
    for omm in (False, True):
        destination = output / ('reference.omm.spv' if omm else 'reference.spv')
        command = [str(compiler), '--target-env', 'spirv1.4', '-V', '-DHYBRID_NO_GUIDE=1',
                   '-I' + str(output / 'Include')]
        if omm:
            command.append('-DZEN_OPACITY_MICROMAP=1')
        command += [str(output / 'Shaders/VoxelGI/hybrid_trace_hardware_buffered.comp'), '-o', str(destination)]
        subprocess.run(command, check=True)
        subprocess.run([str(validator), '--target-env', 'vulkan1.2', str(destination)], check=True)
        modules.append(dict(path=str(destination), sha256=digest(destination), arguments=command))
    files = {str(path.relative_to(output)): digest(path) for path in output.rglob('*') if path.is_file()}
    write_json(output / 'manifest.json', dict(modules=modules, files=files, pixels=int(np.count_nonzero(mask)),
                                             strata=2040, render=[3840, 2160], seed=0x3c6ef372,
                                             builder_sha256=digest(__file__)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, default=Path(os.environ['VULKAN_SDK']))
    main(parser.parse_args())
