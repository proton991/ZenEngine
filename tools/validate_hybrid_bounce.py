"""P5 bounce gate: the checks the P5 exit and the review decisions of 2026-10-10 require, once per provider.

- Fixtures (128^3, uniform .5 albedo, one point light, no environment): cone, ray and 64x1024-sample
  reference captures of the room and thin wall. Gates: one-bounce bias against the independent
  full-image Mitsuba reference on CUDA (tools/ground_truth_bounce.py, checked first against its analytic
  fixtures), shipping bounce limits, thin-wall leakage, and the light-change limits (move, off, emissive
  on/off at frame 8; a continuously orbiting light at frame 64).
- One forward-material fixture with ray bounce: finite output and exact component sums.
- With --sponza, the frozen top and hall cameras (64^3, Papermill only): cone, ray and reference captures.
  Ray bounce is gated on the composed diffuse D_sky + D_bounce; bounce-only errors and the cone
  comparison are reported.

The plan's P5 exit gates the RT tier; the compute tier's results are measurements for the P8 choice, so
Sponza does not gate it. Pass timings come from the debug build with validation and only compare
cone with ray bounce. Limits are frozen; failed gates are reported and return failure.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path
import numpy as np
from compare_hybrid_gi import compare, load
from validate_hybrid_gi import run

TOOLS=Path(__file__).resolve().parent
# Light-change stages, their captured frames, and the frame each is gated at.
STAGES={'move':(1,4,8),'off':(1,4,8),'emissive_on':(1,4,8),'emissive_off':(1,4,8),'orbit':(64,)}
GATED={'move':8,'off':8,'emissive_on':8,'emissive_off':8,'orbit':64}


def profile_cost(prefix):
    profile=json.loads(Path(str(prefix)+'.profile.json').read_text())
    names=('HybridTrace','HybridTemporal','HybridFilter','SceneLighting')
    passes={p['pass']:p['gpu_us'] for p in profile['pass_statistics']
            if p['phase']=='measured' and p['pass'].startswith(names)}
    return dict(summed_pass_median_us=sum(p['median'] or 0 for p in passes.values()),
                pass_median_us={name:p['median'] for name,p in passes.items()})


class IndependentReference:
    """Full-image one-bounce Mitsuba reference on CUDA; no engine voxels or radiance are read.

    CUDA is required rather than falling back to the CPU backend. Results are immutable files, so a
    saved reference is reused when the gate re-analyzes captures."""
    def __init__(self):
        self.mi=None; self.fixtures=None

    def __call__(self, capture, scene, output):
        path=Path(str(output)+'.json')
        if not path.exists():
            if self.mi is None:
                from ground_truth_bounce import self_test, validate_capture
                from ground_truth_mitsuba import setup_mitsuba
                self.mi=setup_mitsuba('cuda'); self.fixtures=self_test(self.mi); self.validate=validate_capture
            self.validate(self.mi,capture,output,scene_path=scene,fixture_gate=self.fixtures)
        r=json.loads(path.read_text())
        return dict(tool='tools/ground_truth_bounce.py',variant=r['variant'],samples=r['samples'],
                    unmatched_pixels=r['primary_mismatch'],analytic_fixtures=r['analytic_fixture_gate'],
                    regions={k:dict(pixels=v['pixels'],bias=v['bias'],rms=v['rms'],excess_rms=v['excess_rms'])
                             for k,v in r['regions'].items()},
                    bias=r['regions']['all_receivers']['bias'],passed=r['bias_gate_passed'])


def thin_wall(prefix):
    _,c=load(prefix)
    floor=(c[:,:,8,3]>0)&(c[:,:,10,1]>.99)
    # Include the wall base: excluding a strip there would hide filter leakage.
    lit=floor&(c[:,:,8,0]<0)
    unlit=floor&(c[:,:,8,0]>0)
    assert lit.any() and unlit.any()
    a=float(c[:,:,3,:3][lit].mean());b=float(c[:,:,3,:3][unlit].mean())
    return dict(lit_mean=a,unlit_mean=b,ratio=b/max(a,1e-20),lit_pixels=int(lit.sum()),unlit_pixels=int(unlit.sum()),
                rejected_lookups=float(c[:,:,2,3][floor].sum()),passed=a>0 and b<=.01*a)


def light_changes(prefix, channel='bounce'):
    report={}
    for stage,frames in STAGES.items():
        base=str(prefix)+'-lights-'+stage
        before_meta,before=load(base+'-before')
        for frame in frames:
            current_meta,current=load(f'{base}-frame{frame}')
            metrics=compare(f'{base}-frame{frame}',base+'-reference',channel=channel)
            mask=current[:,:,8,3]>0
            # History length of the compared channel: bounce alpha, or component 12's w for reflections.
            history=(current[:,:,12,3] if channel=='reflection' else current[:,:,3,3])[mask]
            sky_retained=bool(np.all(current[:,:,7,0][mask]==before[:,:,7,0][mask]))
            radiance_changed=current_meta['radiance_generation']>before_meta['radiance_generation']
            report[f'{stage}_frame{frame}']=dict(metrics=metrics,gated=frame==GATED[stage],sky_history_retained=sky_retained,
                radiance_generation_changed=radiance_changed,bounce_history_mean=float(history.mean()),
                bounce_history_max=float(history.max()),
                passed=all(v['passed'] for v in metrics.values()) and sky_retained and radiance_changed)
    return report


FIXTURES=['point_light_room','thin_wall_light']
CAMERAS=('top','hall')
SPONZA=(('cone','cone',0),('rays','rays',0),('reference','rays',1024))


def capture_fixtures(args, root):
    for source,reference in (('cone',0),('rays',0),('rays',1024)):
        folder=root/('reference' if reference else source)
        run(args.exe,folder,FIXTURES,provider=args.provider,rt=args.provider=='hardware',indirect=1,gpu=args.gpu,
            reference_samples=reference,bounce_source=source,resolution=128,environment_lighting=False,shadows=True,
            light_changes=source=='rays' and reference==0,width=args.width,height=args.height)
    # Forward composition of ray bounce: finite output and exact component sums (asserted by run).
    run(args.exe,root/'forward',['forward_closed_box'],provider=args.provider,rt=args.provider=='hardware',indirect=1,
        gpu=args.gpu,bounce_source='rays',resolution=128,width=args.width,height=args.height)


def capture_sponza(args, root):
    for camera in CAMERAS:
        for name,source,reference in SPONZA:
            subprocess.run([sys.executable,str(TOOLS/'capture_hybrid_gi.py'),'--exe',str(args.exe),'--scene',str(args.sponza),
                            '--output',str(root/'sponza'/f'{camera}-{name}'),'--camera',camera,'--provider',args.provider,
                            '--bounce','1','--bounce-source',source,'--reference-samples',str(reference)]
                           +(['--rt'] if args.provider=='hardware' else []),check=True)


def fixtures(root, independent):
    report={}
    for name in FIXTURES:
        result=dict(shipping=compare(root/'rays'/name,root/'reference'/name,channel='bounce'),
                    cone=compare(root/'cone'/name,root/'reference'/name,channel='bounce'),
                    costs={source:profile_cost(root/source/name) for source in ('cone','rays')},
                    independent_reference=independent(root/'reference'/name,root/'reference/fixtures'/(name+'.gltf'),
                                                      root/'ground-truth'/name),
                    light_changes=light_changes(root/'rays'/name))
        if name=='thin_wall_light':
            result['thin_wall']=thin_wall(root/'rays'/name)
            result['thin_wall_cone']=thin_wall(root/'cone'/name)
        result['passed']=(result['independent_reference']['passed'] and all(m['passed'] for m in result['shipping'].values())
                          and result.get('thin_wall',dict(passed=True))['passed']
                          and all(v['passed'] for v in result['light_changes'].values() if v['gated']))
        report[name]=result
    return report


def sponza(root):
    report={}
    for camera in CAMERAS:
        prefixes={name:root/'sponza'/f'{camera}-{name}' for name,_,_ in SPONZA}
        quality={source:{channel:compare(prefixes[source],prefixes['reference'],channel=channel)
                         for channel in ('diffuse','bounce')} for source in ('cone','rays')}
        _,reference=load(prefixes['reference'])
        mask=reference[:,:,8,3]>0
        sky=float(reference[:,:,1,:3][mask].mean());bounce=float(reference[:,:,3,:3][mask].mean())
        report[camera]=dict(bounce_share=bounce/(sky+bounce),quality=quality,
                            costs={source:profile_cost(prefixes[source]) for source in ('cone','rays')},
                            passed=all(m['passed'] for m in quality['rays']['diffuse'].values()))
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--provider',choices=('hardware','voxel'),required=True)
    parser.add_argument('--gpu')
    parser.add_argument('--capture',action='store_true',help='Generate captures before analysis; otherwise inspect saved captures')
    parser.add_argument('--sponza',type=Path,help='glTF Sample Assets Sponza/glTF/Sponza.gltf; adds the frozen cameras')
    parser.add_argument('--width',type=int,default=960)
    parser.add_argument('--height',type=int,default=540)
    args=parser.parse_args();args.exe=args.exe.resolve();root=args.output.resolve();root.mkdir(parents=True,exist_ok=True)
    # Every capture finishes before the CUDA reference starts, so the two never share the GPU.
    if args.capture:
        capture_fixtures(args,root)
        if args.sponza:
            capture_sponza(args,root)
    report=dict(provider=args.provider,executable_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                fixtures=fixtures(root,IndependentReference()))
    gates=[v['passed'] for v in report['fixtures'].values()]
    if args.sponza:
        report['sponza']=sponza(root)
        if args.provider=='hardware':
            gates+=[v['passed'] for v in report['sponza'].values()]
    report['passed']=all(gates)
    (root/'bounce-results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)
    raise SystemExit(0 if report['passed'] else 1)


if __name__=='__main__':
    main()
