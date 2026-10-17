"""Compare linear reconstructed sky to a raw, converged capture of the same tier.

Channels: sky (D_sky), bounce (D_bounce), or diffuse (D_sky + D_bounce, the shaded irradiance
that static frozen cameras gate once ray-hit bounce is selected).
"""
import argparse
import json
from pathlib import Path
import numpy as np
from hybrid_capture_io import read_bytes

# Capture components holding each signal: raw at the offset, reconstructed one after it.
CHANNELS={'sky':(0,),'bounce':(2,),'diffuse':(0,2),'reflection':(4,)}


def load(prefix):
    m=json.loads(Path(str(prefix)+'.lighting.json').read_text())
    return m,np.frombuffer(read_bytes(str(prefix)+'.hybrid.bin'),'<f4').reshape(m['height'],m['width'],13,4).copy()


def validate_reference(metadata, data, samples=65536):
    """Check the accumulated reference window, including native companion captures.

    Native motion/reset sequences keep a global frame index, so their metadata's
    frame number alone cannot establish how many samples this reference contains.
    """
    per_frame = metadata.get('sample_count', 0)
    valid = data[:, :, 8, 3] > 0
    if (per_frame <= 4 or samples % per_frame or not valid.any()
            or not metadata.get('temporal') or metadata.get('filter_iterations') != 0
            or metadata.get('adaptive_sampling', False)
            or metadata.get('sample_count_max', per_frame) != per_frame):
        raise ValueError('Capture is not an unfiltered fixed-budget temporal reference')
    frames = samples // per_frame
    lengths = data[:, :, 7, 0][valid]
    if not np.isfinite(data).all() or not np.allclose(lengths, frames, rtol=0, atol=1e-3):
        raise ValueError(f'Reference needs {frames} accumulated frames; observed {lengths.min()}..{lengths.max()}')
    return dict(samples_per_frame=per_frame, accumulated_frames=frames, samples=samples)


def compare(candidate,reference,additional_regions=None,channel='sky',required=('all_receivers',)):
    cm,c=load(candidate);rm,r=load(reference)
    assert cm['projection_view_column_major']==rm['projection_view_column_major']
    assert cm['voxel_resolution']==rm['voxel_resolution']
    return compare_data(c,r,rm,additional_regions,channel,required)


def compare_data(c,r,rm,additional_regions=None,channel='sky',required=('all_receivers',)):
    """Report every non-empty region. A required region without pixels is reported as failed
    with NaN metrics, so callers combining regions with all() cannot pass it vacuously."""
    valid=r[:,:,8,3]>0
    regions={'all_receivers':valid,'sponza_floor':valid&(r[:,:,10,1]>.99)&(r[:,:,8,1]<-.12)}
    if additional_regions:
        regions.update({name: valid & mask for name,mask in additional_regions.items()})
    unknown=set(required)-set(regions)
    if unknown:raise ValueError(f'Unknown required regions: {sorted(unknown)}')
    report={}
    for name,mask in regions.items():
        if not mask.any():
            if name in required:
                nan=float('nan')
                report[name]=dict(pixels=0,reference_mean=nan,bias=nan,rms=nan,p99=nan,unexpected_zeros=0,passed=False)
            continue
        converged=1 if rm.get("sample_count",0)>4 else 0
        expected=sum(r[:,:,o+converged,:3] for o in CHANNELS[channel])[mask]
        actual=sum(c[:,:,o+1,:3] for o in CHANNELS[channel])[mask];mean=expected.mean()
        error=actual-expected
        denominator=max(float(mean),1e-20)
        bias=float(error.mean()/denominator)
        rms=float(np.sqrt((error**2).mean())/denominator)
        p99=float(np.quantile(abs(error),.99)/denominator)
        zeros=int(np.sum((actual.mean(1)==0)&(expected.mean(1)>=.1*mean)&(expected.mean(1)>0)))
        report[name]=dict(pixels=int(mask.sum()),reference_mean=float(mean),bias=bias,rms=rms,p99=p99,unexpected_zeros=zeros,
                          passed=abs(bias)<=.02 and rms<=.08 and p99<=.2 and zeros==0)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('candidate',type=Path);p.add_argument('reference',type=Path);p.add_argument('--output',type=Path)
    p.add_argument('--channel',choices=tuple(CHANNELS),default='sky')
    a=p.parse_args();report=compare(a.candidate,a.reference,channel=a.channel);text=json.dumps(report,indent=2);print(text)
    if a.output:a.output.write_text(text+'\n')
