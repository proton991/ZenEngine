"""Compare linear reconstructed sky to a raw, converged capture of the same tier."""
import argparse
import json
from pathlib import Path
import numpy as np


def load(prefix):
    m=json.loads(Path(str(prefix)+'.lighting.json').read_text())
    return m,np.fromfile(str(prefix)+'.hybrid.bin','<f4').reshape(m['height'],m['width'],13,4)


def compare(candidate,reference):
    cm,c=load(candidate);rm,r=load(reference)
    assert cm['projection_view_column_major']==rm['projection_view_column_major']
    assert cm['voxel_resolution']==rm['voxel_resolution']
    valid=r[:,:,8,3]>0
    regions={'all_receivers':valid,'sponza_floor':valid&(r[:,:,10,1]>.99)&(r[:,:,8,1]<-.12)}
    report={}
    for name,mask in regions.items():
        if not mask.any():continue
        expected=r[:,:,1 if rm.get("sample_count",0)>4 else 0,:3][mask];actual=c[:,:,1,:3][mask];mean=expected.mean()
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
    a=p.parse_args();report=compare(a.candidate,a.reference);text=json.dumps(report,indent=2);print(text)
    if a.output:a.output.write_text(text+'\n')
