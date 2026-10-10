"""P4 bright-source experiment: replace compact bright cube regions by explicit lights.

This is an opt-in experiment, not a shipping estimator. Source energy uses texel
quadrature and directions use a centroid or a finite importance quadrature table;
finite-source shadow penumbrae are therefore approximated. Compare against the unchanged
full-environment reference. The residual retains four rays, and each source has one
dedicated visibility ray. Optional separate reconstruction runs the residual and source
group through independent copies of the existing reconstruction and adds their outputs.

The generated shader and metadata stay under --output. Only its compiled SPIR-V is
temporarily installed; the original bytes are restored in finally. Run serially with
all engine captures, shader builds and other configuration-mutating tools.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

import numpy as np

from compare_hybrid_gi import compare_data, load
from ground_truth_sweep import ENVIRONMENTS, ROOT


def extract(cube, count=8, threshold=8, max_radius=6, area_samples=0):
    size = cube.shape[1]
    u, v = np.meshgrid((np.arange(size)+.5)*2/size-1, (np.arange(size)+.5)*2/size-1)
    one = np.ones_like(u)
    directions = np.stack([np.stack(x, -1) for x in ((one,-v,-u),(-one,-v,u),(u,one,v),
                                                   (u,-one,-v),(u,-v,one),(-u,-v,-one))])
    length = np.linalg.norm(directions, axis=-1)
    directions /= length[..., None]
    solid = 4/(size*size*length**3)
    luminance = cube[..., :3] @ [.2126, .7152, .0722]
    cutoff = threshold*float(np.sum(luminance*solid)/(4*np.pi))
    bright = luminance > cutoff
    components = []
    for face in range(6):
        remaining = set(np.flatnonzero(bright[face]).tolist())
        while remaining:
            seed = min(remaining)
            remaining.remove(seed)
            pending, cells = [seed], []
            while pending:
                p = pending.pop()
                cells.append(p)
                y, x = divmod(p, size)
                for dy, dx in ((-1,0),(1,0),(0,-1),(0,1)):
                    if 0 <= y+dy < size and 0 <= x+dx < size:
                        q = (y+dy)*size+x+dx
                        if q in remaining:
                            remaining.remove(q)
                            pending.append(q)
            ids = np.array(cells)+face*size*size
            weight = (luminance*solid).ravel()[ids]
            center = np.sum(directions.reshape(-1,3)[ids]*weight[:,None], axis=0)
            center /= np.linalg.norm(center)
            radius = np.arccos(np.clip((directions.reshape(-1,3)[ids]@center).min(), -1, 1))+2/size
            if np.degrees(radius) <= max_radius:
                components.append((float(weight.sum()), center, radius))
    components.sort(key=lambda c: c[0], reverse=True)
    assigned = np.zeros(luminance.shape, bool)
    sources = []
    for _, direction, radius in components[:count]:
        selected = (directions@direction >= np.cos(radius)) & ~assigned
        energy = np.sum(cube[..., :3][selected]*solid[selected,None], axis=0)
        assigned |= selected
        source = dict(direction=direction.tolist(), cosine=float(np.cos(radius)),
                      radius_degrees=float(np.degrees(radius)), energy=energy.tolist())
        if area_samples and np.any(selected):
            rgb, light = cube[..., :3][selected], luminance[selected]
            weights = light*solid[selected]
            picks = np.searchsorted(np.cumsum(weights), (np.arange(area_samples)+.5)*weights.sum()/area_samples)
            source['area_directions'] = directions[selected][picks].tolist()
            source['area_weights'] = (rgb[picks]*weights.sum()/np.maximum(light[picks,None],1e-30)).tolist()
        if np.any(energy > 0):
            sources.append(source)
    total = float(np.sum(luminance*solid))
    return dict(sources=sources, luminance_energy_fraction=float(np.sum(luminance[assigned]*solid[assigned])/total)
                if total else 0, threshold=cutoff, max_radius_degrees=max_radius)


def source_constants(prototype):
    sources = prototype['sources']
    header = 'const uint sourceCount = '+str(len(sources))+'u;\n'
    # GLSL disallows a zero-sized array; the unused element is harmless.
    directions = sources or [dict(direction=[0,1,0],cosine=1,energy=[0,0,0])]
    fmt = lambda values: ','.join(f'{v:.9e}' for v in values)
    header += 'const vec4 sourceDirections[] = vec4[]('+','.join(
        'vec4('+fmt(s['direction']+[s['cosine']])+')' for s in directions)+');\n'
    header += 'const vec3 sourceEnergy[] = vec3[]('+','.join(
        'vec3('+fmt(s['energy'])+')' for s in directions)+');\n'
    return header


def shader(prototype, component='combined'):
    sources = prototype['sources']
    header = source_constants(prototype)
    fmt = lambda values: ','.join(f'{v:.9e}' for v in values)
    area_samples = len(sources[0].get('area_directions', [])) if sources else 0
    if area_samples:
        header += 'const vec3 areaDirections[] = vec3[]('+','.join(
            'vec3('+fmt(d)+')' for s in sources for d in s['area_directions'])+');\n'
        header += 'const vec3 areaWeights[] = vec3[]('+','.join(
            'vec3('+fmt(d)+')' for s in sources for d in s['area_weights'])+');\n'
    header += '''
vec3 ResidualRadiance(vec3 world)
{
    vec3 local=EnvironmentDirection(world);
    for(uint k=0u;k<sourceCount;++k)
        if(dot(local,sourceDirections[k].xyz)>=sourceDirections[k].w) return vec3(0);
    return textureLod(coneEnvironmentMap,local,0).rgb;
}
vec3 SourceWorldDirection(vec3 direction)
{
    vec4 q=vec4(-sceneUbo.environmentOrientation.xyz,sceneUbo.environmentOrientation.w);
    direction+=2.0*cross(q.xyz,cross(q.xyz,direction)+q.w*direction);
    float c=cos(sceneUbo.environment.y),s=sin(sceneUbo.environment.y);
    return vec3(c*direction.x+s*direction.z,direction.y,-s*direction.x+c*direction.z);
}
'''
    trace = (ROOT/'Data/Shaders/VoxelGI/hybrid_trace.glsl').read_text()
    trace = trace.replace('void main()', header+'\nvoid main()')
    trace = trace.replace('textureLod(coneEnvironmentMap,EnvironmentDirection(w),0).rgb', 'ResidualRadiance(w)')
    trace = trace.replace('sky/=float(hybrid.sampling.y);', '''sky/=float(hybrid.sampling.y);
        for(uint k=0u;k<sourceCount;++k)
        {
            vec3 w=SourceWorldDirection(sourceDirections[k].xyz);
            if(dot(n,w)>0.0 && dot(ng,w)>0.0 && !HybridTraceRay(voxelOpacity,origin,w,1e20).hit)
                sky.rgb+=sourceEnergy[k]*max(dot(n,w),0.0)/HYBRID_PI
                    *sceneUbo.environment.x*sceneUbo.environment.z*gi.lighting.y;
        }''')
    if area_samples:
        trace = trace.replace('vec3 w=SourceWorldDirection(sourceDirections[k].xyz);',
                              f'uint pick=k*{area_samples}u+min(uint(HybridSample(uvec2(p),hybrid.sampling.x,k+37u).x*{area_samples}.0),{area_samples-1}u);\n'
                              '            vec3 w=SourceWorldDirection(areaDirections[pick]);')
        trace = trace.replace('sky.rgb+=sourceEnergy[k]*', 'sky.rgb+=areaWeights[pick]*')
    if component == 'residual':
        trace = trace.replace('k<sourceCount;++k)\n        {', 'k<0u;++k)\n        {')
    elif component == 'sources':
        trace = trace.replace('return textureLod(coneEnvironmentMap,local,0).rgb;', 'return vec3(0);')
    return '#version 460\n#extension GL_GOOGLE_include_directive : require\n#extension GL_EXT_ray_query : require\n#define HYBRID_HARDWARE_PROVIDER\n'+trace


def residual_columns(prototype):
    source = (ROOT/'Data/Shaders/VoxelGI/environment_columns.comp').read_text()
    source = source.replace('void main()', source_constants(prototype)+'\nvoid main()')
    source = source.replace('sum += max(dot(radiance,', '''vec3 proposal=radiance;
            for(uint k=0u;k<sourceCount;++k)
                if(dot(direction,sourceDirections[k].xyz)>=sourceDirections[k].w) proposal=vec3(0);
            sum += max(dot(proposal,''')
    return source


def run(args):
    names = ['hybrid_trace_hardware'] + (['environment_columns'] if args.residual_proposal else [])
    binaries = {name: ROOT/f'Data/SpvShaders/VoxelGI/{name}.comp.spv' for name in names}
    originals = {name: path.read_bytes() for name, path in binaries.items()}
    installed = dict(originals)
    report = {}
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        for environment in args.environments:
            reference = args.references/f'{environment}-hall-reference'
            metadata, _ = load(reference)
            size = metadata['environment_cube_size']
            cube = np.fromfile(str(reference)+'.environment.bin', '<f4').reshape(6,size,size,4)
            prototype = extract(cube, area_samples=args.area_samples)
            components = ('residual', 'sources') if args.separate_reconstruction else ('combined',)
            combined = None
            for component in components:
                for name, binary in binaries.items():
                    source = args.output/f'{environment}-{component}-{name}.comp'
                    source.write_text(shader(prototype, component) if name == 'hybrid_trace_hardware' else residual_columns(prototype))
                    compiled = source.with_suffix('.spv')
                    subprocess.run([str(args.compiler),'--target-env','spirv1.3','-V',str(source),'-o',str(compiled),
                                    '-I'+str(ROOT/'ZenCore/Include'),'-I'+str(ROOT/'Data/Shaders/VoxelGI')], check=True)
                    if binary.read_bytes() != installed[name]:
                        raise RuntimeError('External shader edit; refusing to overwrite')
                    installed[name] = compiled.read_bytes()
                    binary.write_bytes(installed[name])
                prefix = args.output/f'{environment}-{component}-hall'
                subprocess.run([sys.executable,str(ROOT/'tools/capture_hybrid_gi.py'),'--exe',str(args.exe),
                                '--scene',str(args.scene),'--output',str(prefix),'--camera','hall','--provider','hardware','--rt',
                                '--environment-texture',ENVIRONMENTS[environment]], check=True)
                _, captured = load(prefix)
                if combined is None:
                    combined = captured.copy()
                else:
                    combined[:, :, 1, :3] += captured[:, :, 1, :3]
            reference_metadata, reference_data = load(reference)
            prototype['reconstruction'] = list(components)
            prototype['shipping_gate'] = compare_data(combined, reference_data, reference_metadata)
            np.savez_compressed(args.output/f'{environment}-combined.npz', sky=combined[:, :, 1, :3])
            report[environment] = prototype
            (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
            print(environment,json.dumps(prototype['shipping_gate']),flush=True)
    finally:
        conflicts = []
        for name, binary in binaries.items():
            if binary.read_bytes() != installed[name]:
                conflicts.append(str(binary))
            else:
                binary.write_bytes(originals[name])
        if conflicts:
            raise RuntimeError(f'External shader edits; left untouched: {conflicts}')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--scene',type=Path,required=True)
    parser.add_argument('--references',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--compiler',type=Path,required=True)
    parser.add_argument('--area-samples',type=int,default=0,
                        help='Finite source quadrature table size; one visibility ray per source and frame (0 uses its centroid)')
    parser.add_argument('--residual-proposal',action='store_true',help='Build the four-ray proposal from the residual environment')
    parser.add_argument('--separate-reconstruction',action='store_true',
                        help='Reconstruct residual and sources separately, then compare their linear sum (two diagnostic captures)')
    parser.add_argument('--environments',nargs='+',choices=ENVIRONMENTS,
                        default=['qwantani','hotel','carpentry','studio'])
    args = parser.parse_args()
    if not 0 <= args.area_samples <= 64:
        parser.error('area-samples must be 0..64; large constant shader tables can exceed GPU execution limits')
    run(args)
