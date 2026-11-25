"""Check the production GLSL probe direction sampler against solid-angle quadrature.

Requires numpy and moderngl. This is an isolated numerical check of the proposal, with
a constant environment and a uniform octahedral CDF. Native ZenApp fixtures cover the
real environment distributions, cached guide construction and traced HDR transport.
"""
import argparse
import json
import re
from itertools import product
from pathlib import Path

import moderngl
import numpy as np


ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / 'Data/Shaders'
BATCHES = 32768


def function(source, name):
    signature = re.search(r'^(?:uint|float|mat3|vec[234])\s+' + name + r'\s*\(', source, re.M)
    if not signature:
        raise ValueError(f'Cannot find shader function {name}')
    start = signature.start()
    brace = source.index('{', start)
    level = 1
    for i in range(brace + 1, len(source)):
        level += (source[i] == '{') - (source[i] == '}')
        if not level:
            return source[start:i + 1]
    raise ValueError(name)


def kernel():
    header = """#version 450
layout(local_size_x=64) in;
layout(std430,binding=0) writeonly buffer Result {vec4 samples[];};
const float HYBRID_PI=3.141592653589793;
struct ScreenProbe {vec4 normal;};
struct ProbeLobe {vec3 axis; float concentration; float weight;};
struct Options {uint residualEnvironment;}; const Options screenProbe=Options(0u);
uniform int fixture;
uniform int cold;
uniform bool useEnvironment;
uniform bool hasLearned;
uniform bool densityCheck;
bool ProbeEnvironmentSampling(){return useEnvironment;}
float ProbeCDFBoundary(uint index,uint boundary){return float(boundary)/64.;}
float EnvironmentPDF(vec3 d){return 1./(4.*HYBRID_PI);}
float EnvironmentResidualPDF(vec3 d,out bool src){src=false;return EnvironmentPDF(d);}
vec3 SampleEnvironmentDistribution(vec2 u,bool residual,out float pdf){
    float z=2.*u.y-1., r=sqrt(max(1.-z*z,0.));pdf=1./(4.*HYBRID_PI);
    return vec3(r*cos(2.*HYBRID_PI*u.x),r*sin(2.*HYBRID_PI*u.x),z);
}
"""
    parts = [header]
    lanes = (ROOT / 'ZenCore/Include/Graphics/Shared/ScreenProbeGI.h').read_text()
    parts.append(re.search(r'^#define\s+ZEN_SCREEN_PROBE_RAYS\s+\d+u?\b', lanes, re.M)[0])
    for path, constants in (
        ('VoxelGI/screen_probe_trace.glsl', ('kProbeEnvironmentLanes', 'kProbeColdFrames')),
        ('VoxelGI/probe_light_sampling.glsl', ('kProbeLightAnisotropy',)),
    ):
        source = (SHADERS / path).read_text()
        parts.extend(re.search(r'^const\s+\w+\s+' + name + r'\s*=\s*[^;]+;', source, re.M)[0] for name in constants)
    for path, names in (
        ('Common/gbuffer.glsl', ['GBufferSignNotZero', 'EncodeGBufferNormal', 'DecodeGBufferNormal']),
        ('VoxelGI/hybrid_sampling.glsl', ['HybridHash', 'HybridBasis', 'HybridHenyeyGreensteinPDF', 'HybridHenyeyGreensteinSample']),
        ('VoxelGI/probe_light_sampling.glsl', ['ProbeLightPDF', 'ProbeLightSample']),
        ('VoxelGI/screen_probe.glsl', ['ProbeOctBin', 'ProbeOctDensity', 'ProbeLightProjection']),
        ('VoxelGI/screen_probe_trace.glsl', ['ProbeSampleDirection']),
    ):
        source = (SHADERS / path).read_text()
        for name in names:
            body = function(source, name)
            if name == 'ProbeSampleDirection':
                # Evaluate the same production density on an independent equal-area
                # hemisphere grid. The shader only estimates positive projected cosine.
                boundary = re.search(r'uint\s+bin\s*=\s*ProbeOctBin\(direction\);', body).start()
                grid = """if(densityCheck){
                    uint id=gl_GlobalInvocationID.x;
                    float z=(float(id)+.5)/float(gl_NumWorkGroups.x*gl_WorkGroupSize.x);
                    float phi=float(bitfieldReverse(id))*(2.*HYBRID_PI/4294967296.);
                    float r=sqrt(1.-z*z);direction=vec3(r*cos(phi),r*sin(phi),z);
                }\n"""
                body = body[:boundary] + grid + body[boundary:]
            parts.append(body)
    parts.append("""
void main(){
    uint i=gl_GlobalInvocationID.x, lane=i%uint(ZEN_SCREEN_PROBE_RAYS);
    ScreenProbe probe=ScreenProbe(vec4(0,0,1,cold!=0 ? 1. : 32.));
    vec3 axes[8];float weights[8],lobes[8], mass=0.;
    uint count=fixture==0 ? 0u : (fixture==2 || fixture==3 ? 3u : 1u);
    for(uint k=0u;k<8u;++k){
        vec3 a=fixture==4 ? vec3(1,0,.02) : vec3(.6,0,.8);
        if(fixture==2 && k==1u)a=vec3(-.8,0,.6);
        if(fixture==2 && k==2u)a=vec3(0,1,-.08);
        axes[k]=normalize(a); lobes[k]=.88+.025*float(min(k,2u));
        weights[k]=k<count ? ProbeLightProjection(axes[k].z)/(1.+float(k)*float(k)) : 0.;
        mass+=weights[k];
    }
    for(uint k=0u;k<8u;++k)weights[k]/=max(mass,1e-20);
    ProbeLobe learned[3];for(uint k=0u;k<3u;++k)
        learned[k]=ProbeLobe(normalize(vec3(-.3,.2,1)),.82,k==0u ? 1. : 0.);
    uvec2 rnd=uvec2(HybridHash(i ^ 0x815421c7u),HybridHash(i ^ 0x942083bdu));
    samples[i]=ProbeSampleDirection(0u,lane,probe,vec2(rnd>>8u)/16777216.,mass,axes,weights,lobes,learned,hasLearned);
}
""")
    return '\n'.join(parts)


def reference(axis):
    # Integrate positive projected cosine over a five-degree cap for the bright patch.
    nodes, weights = np.polynomial.legendre.leggauss(96)
    edge = np.cos(np.deg2rad(5))
    z = (1 + edge + nodes * (1 - edge)) * .5
    phi = (np.arange(768) + .5) * 2 * np.pi / 768
    height = z[:, None] * axis[2] + np.sqrt(1 - z[:, None] ** 2) * axis[0] * np.cos(phi)
    return np.pi * (1 - edge) * np.sum(weights * np.maximum(height, 0).mean(axis=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    context = moderngl.create_standalone_context(require=450)
    source = kernel()
    rays = int(re.search(r'#define\s+ZEN_SCREEN_PROBE_RAYS\s+(\d+)', source)[1])
    shader = context.compute_shader(source)
    output = context.buffer(reserve=BATCHES * rays * 16)
    output.bind_to_storage_buffer(0)
    result = {}
    for fixture, name in enumerate(('no-lamp', 'one-lamp', 'unequal', 'coincident', 'grazing')):
        axis = np.array([1., 0., .02]) if fixture == 4 else np.array([.6, 0., .8])
        axis /= np.linalg.norm(axis)
        for cold, environment, learned in product((0, 1), repeat=3):
            shader['fixture'].value = fixture
            shader['cold'].value = cold
            shader['useEnvironment'].value = bool(environment)
            shader['hasLearned'].value = bool(learned)
            shader['densityCheck'].value = False
            shader.run(BATCHES * rays // 64)
            samples = np.frombuffer(output.read(), np.float32).reshape(-1, 4)
            direction, pdf = samples[:, :3], samples[:, 3]
            if not np.all(np.isfinite(samples)) or np.any(pdf <= 0):
                raise ValueError('Invalid proposal or density')
            cosine = np.maximum(direction[:, 2], 0)
            angles = np.degrees(np.arccos(np.clip(direction @ axis, -1, 1)))
            row = {}
            for label, estimate, exact in (
                ('constant', cosine / pdf, np.pi),
                ('bright_cap', cosine * (angles < 5) / pdf, reference(axis)),
            ):
                batches = estimate.reshape(BATCHES, rays).mean(axis=1, dtype=np.float64)
                error = float(batches.std(ddof=1) / np.sqrt(BATCHES))
                mean = float(batches.mean())
                row[label] = dict(mean=mean, expected=float(exact), stderr=error, passed=bool(abs(mean - exact) < 5 * error))
            shader['densityCheck'].value = True
            shader.run(BATCHES * rays // 64)
            grid = np.frombuffer(output.read(), np.float32).reshape(-1, 4)
            grid_angle = np.degrees(np.arccos(np.clip(grid[:, :3] @ axis, -1, 1)))
            histogram = []
            for lo, hi in zip((0, 3, 5, 10, 30), (3, 5, 10, 30, 181)):
                in_bin = (angles >= lo) & (angles < hi) & (direction[:, 2] > 0)
                counts = in_bin.reshape(BATCHES, rays).mean(axis=1)
                observed = float(counts.mean())
                expected = float(2 * np.pi * np.mean(grid[:, 3] * ((grid_angle >= lo) & (grid_angle < hi))))
                stderr = float(counts.std(ddof=1) / np.sqrt(BATCHES))
                histogram.append(dict(degrees=[lo, min(hi, 180)], observed=observed, integrated=expected,
                                      passed=abs(observed - expected) < 5 * stderr + .0002))
            row['upper_hemisphere_bins'] = histogram
            result[f'{name}-{"cold" if cold else "warm"}-env{environment}-learned{learned}'] = row
    report = dict(renderer=context.info['GL_RENDERER'], batches=BATCHES, results=result)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    passed = all(row['constant']['passed'] and row['bright_cap']['passed']
                 and all(b['passed'] for b in row['upper_hemisphere_bins']) for row in result.values())
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
