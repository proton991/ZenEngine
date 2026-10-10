"""P6 reflections: native fixtures, independent GGX lobe reference, composition, motion, light changes, cost.

The RT gate uses the frozen glossy S/hit-fraction limit (2% absolute) against the independent CUDA
lobe reference (tools/ground_truth_specular.py, every second pixel), open PBR and static shipping
limits, motion captures taken without a settling frame, and the light-change limits on the reflection
channel. The reflected-emission comparison has no frozen limit and is reported. Compute results inform
the P8 choice. Below-cutoff (roughness .1) noise is reported separately from the .2 glossy fixture.
Capture runners restore engine.cfg and reject all validation errors; every capture finishes before the
CUDA reference runs. Use --capture once, then re-analyze saved data.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from compare_hybrid_gi import compare, load
from validate_hybrid_gi import run
from validate_hybrid_bounce import light_changes, profile_cost


def provenance(exe):
    shaders=Path(__file__).resolve().parents[1]/'Data/SpvShaders'
    return dict(executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                shaders={p.relative_to(shaders).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(shaders.rglob('*.spv'))
                         if p.relative_to(shaders).parts[0] in ('VoxelGI','SceneRenderer')})


def metrics(actual, expected):
    error=actual-expected
    mean=max(float(expected.mean()),1e-20)
    bias=float(error.mean()/mean)
    rms=float(np.sqrt(np.mean(error**2))/mean)
    p99=float(np.quantile(abs(error),.99)/mean)
    return dict(bias=bias,rms=rms,p99=p99,passed=abs(bias)<=.02 and rms<=.08 and p99<=.2)


class LobeReference:
    """Independent GGX lobe reference on CUDA; saved results are immutable and reused on re-analysis."""
    def __init__(self, root):
        self.root=root/'cuda-reference'; self.mi=None; self.fixtures=None

    def __call__(self, prefix, emissive=False):
        output=self.root/prefix.name
        path=Path(str(output)+'.json')
        if not path.exists():
            if self.mi is None:
                from ground_truth_mitsuba import setup_mitsuba
                from ground_truth_specular import self_test
                self.mi=setup_mitsuba('cuda'); self.fixtures=self_test(self.mi)
            from ground_truth_specular import compare_capture
            compare_capture(self.mi,prefix,prefix.parent/'fixtures'/(prefix.name+'.gltf'),output,
                            samples=16384 if emissive else 65536,emissive=emissive,fixture_gate=self.fixtures)
        return json.loads(path.read_text())


def lighting(prefix):
    metadata=json.loads(Path(str(prefix)+'.lighting.json').read_text())
    return np.fromfile(str(prefix)+'.lighting.bin','<f4').reshape(metadata['height'],metadata['width'],7,4)


def reflection_lag(candidate, reference, floor):
    # Fit horizontal displacement of the reflection on the floor during lateral camera motion.
    # Noise increases the residual; a trail behind the camera's motion shifts its best fit.
    c=np.where(floor,candidate[:,:,5,:3].mean(2),0)
    r=np.where(floor,reference[:,:,5,:3].mean(2),0)
    costs={shift:float(np.mean((c[:,16+shift:c.shape[1]-16+shift]-r[:,16:-16])**2))
           for shift in range(-12,13)}
    return min(costs,key=costs.get)


def enclosed(prefix):
    _,capture=load(prefix)
    mask=capture[:,:,8,3]>0
    sky=float(capture[:,:,1,:3][mask].max())
    s=float(capture[:,:,5,3][mask].max())
    h=float(capture[:,:,5,:3][mask].mean())
    specular=float(lighting(prefix)[:,:,3,:3][mask].mean())
    return dict(sky_max=sky,S_max=s,reflected_mean=h,specular_mean=specular,
                passed=sky<=1e-6 and s==0 and h>0 and specular>0)


def capture(args):
    options=dict(provider=args.provider,rt=args.provider=='hardware',indirect=1,resolution=128,shadows=True,
                 width=args.width,height=args.height)
    # Native captures finish before any independent CPU work; all config mutations are sequential.
    run(args.exe,args.output/'shipping',['glossy_floor','glossy_smooth'],reflection_motion=True,**options)
    run(args.exe,args.output/'shipping',['glossy_closed','forward_glossy_closed','glossy_open'],**options)
    run(args.exe,args.output/'reference',['glossy_floor','glossy_smooth','glossy_closed'],reference_samples=1024,**options)
    run(args.exe,args.output/'occlusion',['glossy_floor','glossy_closed'],reflections=False,**options)
    run(args.exe,args.output/'pbr',['glossy_open'],mode=2,**options)
    run(args.exe,args.output/'emissive',['glossy_emissive'],environment_lighting=False,reference_samples=1024,**options)
    run(args.exe,args.output/'zero-indirect',['glossy_closed'],**dict(options,indirect=0))
    # Moved, switched-off and continuously orbiting light near the glossy floor (P6 review).
    run(args.exe,args.output/'lights',['glossy_floor'],light_changes=True,**options)


def analyze(root):
    reference=LobeReference(root)
    report=dict(lobe_reference=reference(root/'reference/glossy_floor'))
    report['lobe_reference']['passed']=report['lobe_reference']['S']['passed']
    report['static']={name:compare(root/'shipping'/name,root/'reference'/name,channel='reflection')
                      for name in ('glossy_floor','glossy_smooth','glossy_closed')}
    report['enclosed']={name:enclosed(root/'shipping'/name) for name in ('glossy_closed','forward_glossy_closed')}
    _,opened=load(root/'shipping/glossy_open')
    mask=opened[:,:,8,3]>0
    report['open_pbr']=metrics(lighting(root/'shipping/glossy_open')[:,:,3,:3][mask],
                               lighting(root/'pbr/glossy_open')[:,:,3,:3][mask])
    report['open_pbr']['reflected_max']=float(opened[:,:,5,:3][mask].max())
    report['open_pbr']['passed'] &= report['open_pbr']['reflected_max']==0
    _,zero=load(root/'zero-indirect/glossy_closed')
    report['zero_indirect']=dict(reflected_max=float(zero[:,:,5,:3].max()),passed=bool(np.all(zero[:,:,5,:3]==0)))
    # No frozen limit covers reflected emission: report the cache approximation against triangles.
    report['emissive_reference']=reference(root/'emissive/glossy_emissive',emissive=True)
    if (root/'lights').exists():
        report['light_changes']=light_changes(root/'lights/glossy_floor',channel='reflection')
    report['motion']={}
    for name in ('glossy_floor','glossy_smooth'):
        report['motion'][name]={}
        for frame in (8,16,24,32):
            p=root/'shipping'/f'{name}-motion-frame{frame}'
            r=Path(str(p)+'-reference')
            _,c=load(p); _,reference=load(r)
            valid=c[:,:,8,3]>0
            # Report the error in the reflected channel, including newly exposed pixels.
            result=compare(p,r,channel='reflection')
            result['history_max']=float(c[:,:,12,3][valid].max())
            # A stale bright trail appears where the current reference is dark. Compare the current
            # reference, without aligning the reflection back to its old screen position.
            floor=valid&(c[:,:,10,1]>.99)
            result['floor_history_max']=float(c[:,:,12,3][floor].max())
            result['reflection_lag_pixels']=reflection_lag(c,reference,floor)
            result['motion_passed']=abs(result['reflection_lag_pixels'])<=1
            if name=='glossy_smooth':result['motion_passed'] &= result['floor_history_max']<=4
            dark=floor&(reference[:,:,5,:3].mean(2)<.01*reference[:,:,5,:3][floor].mean())
            result['dark_region_radiance']=float(c[:,:,5,:3][dark].mean()) if dark.any() else 0
            report['motion'][name][str(frame)]=result
    report['cost']={name:{mode:profile_cost(root/mode/name) for mode in ('shipping','occlusion')}
                    for name in ('glossy_floor','glossy_closed')}
    # P6's below-cutoff reflection detail is explicitly limited; report that stress case in full.
    gates=[report['lobe_reference']['passed'],report['open_pbr']['passed'],report['zero_indirect']['passed']]
    gates += [v['passed'] for v in report.get('light_changes',{}).values() if v['gated']]
    gates.append('light_changes' in report)
    gates += [v['passed'] for v in report['enclosed'].values()]
    gates += [v['passed'] for name in ('glossy_floor','glossy_closed') for v in report['static'][name].values()]
    gates += [v['motion_passed'] for frames in report['motion'].values() for v in frames.values()]
    report['passed']=bool(all(gates))
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--provider',choices=('hardware','voxel'),required=True)
    parser.add_argument('--capture',action='store_true')
    parser.add_argument('--width',type=int,default=960)
    parser.add_argument('--height',type=int,default=540)
    args=parser.parse_args();args.output=args.output.resolve();args.exe=args.exe.resolve()
    args.output.mkdir(parents=True,exist_ok=True)
    inputs=args.output/'capture-inputs.json'
    if args.capture:
        before=provenance(args.exe)
        inputs.write_text(json.dumps(before,indent=2)+'\n')
        capture(args)
        assert provenance(args.exe)==before,'Executable or shaders changed during capture'
    report=analyze(args.output);report['provider']=args.provider
    if inputs.exists():report['provenance']=json.loads(inputs.read_text())
    (args.output/'reflection-results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)
    raise SystemExit(0 if report['passed'] or args.provider=='voxel' else 1)


if __name__=='__main__':
    main()
