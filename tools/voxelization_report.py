"""Raw-volume slices and explicit regional comparisons for the V0 evidence report."""
import argparse
import json
from pathlib import Path
import struct
from voxelization_fixtures import rgba_png


def read_volume(prefix):
    metadata=json.loads(Path(str(prefix)+'.json').read_text())
    n=metadata['resolution']
    cells={}
    for i,values in enumerate(struct.iter_unpack('<4I4f',Path(str(prefix)+'.voxels.bin').read_bytes())):
        if values[3]:
            cells[(i%n,i//n%n,i//(n*n))]=values
    return metadata,cells


def slice_image(path,n,cells,axis,depth,comparison=None):
    x,y=((axis+1)%3,(axis+2)%3) if axis!=2 else (0,1)
    scale=max(1,512//n)
    pixels=[]
    for row in reversed(range(n)):
        line=[]
        for column in range(n):
            cell=[0,0,0];cell[x],cell[y],cell[axis]=column,row,depth;cell=tuple(cell)
            color=[12,12,12,255]
            if cell in cells:
                packed=cells[cell][1]
                color=[round((((packed>>(i*8))&255)/255)**(1/2.2)*255) for i in range(3)]+[255]
                if comparison is not None and cell not in comparison:
                    color=[255,35,35,255]
            elif comparison is not None and cell in comparison:
                color=[255,220,20,255]
            line.extend([color]*scale)
        pixels.extend(line*scale)
    path.write_bytes(rgba_png(n*scale,n*scale,pixels))


def sponza_regions(prefix, n, cells, reference):
    """Reproducible spatial bands, not a semantic segmentation of the asset."""
    comparison=json.loads(Path(str(prefix)+'.comparison.json').read_text())
    # Coordinates were inspected on the padded 128-cell Sponza slices at z=57/72
    # and y=41/49/66. Scale by normalized grid coordinates; publish every bound.
    boxes={
        'columns': ((29,43,54),(33,55,61)),
        'arches': ((29,54,54),(46,61,61)),
        'fabrics': ((30,44,68),(104,60,78)),
        'upper_gallery_rails': ((30,71,54),(100,78,62)),
        'floor_wall_contacts': ((20,38,42),(108,44,56)),
    }
    regions={}
    for name,(low,high) in boxes.items():
        low,high=tuple(v*n//128 for v in low),tuple(v*n//128 for v in high)
        def inside(cell):
            return all(low[i]<=cell[i]<high[i] for i in range(3))
        result=dict(min_inclusive=low,max_exclusive=high,
                    occupied=sum(inside(cell) for cell in cells),
                    reference_occupied=sum(inside(cell) for cell in reference))
        for label,source in (('engine',comparison),('reference',comparison['reference'])):
            for kind in ('missing','extra'):
                result[label+'_'+kind]=sum(inside(cell) for cell in source[kind+'_cells'])
        regions[name]=result
    Path(str(prefix)+'.regions.json').write_text(json.dumps(regions,indent=2))
    print(json.dumps(regions,indent=2))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix',type=Path)
    parser.add_argument('--other',type=Path)
    parser.add_argument('--sponza-regions',action='store_true')
    args=parser.parse_args()
    metadata,cells=read_volume(args.prefix)
    n=metadata['resolution']
    other=None
    if args.other:
        other_meta,other=read_volume(args.other)
        assert all(metadata[k]==other_meta[k] for k in ('resolution','grid_min','voxel_size','triangle_count'))
        differing=[cell for cell in cells.keys()|other.keys() if cells.get(cell)!=other.get(cell)]
        result=dict(occupied=len(cells),other_occupied=len(other),missing=len(other.keys()-cells.keys()),
                    extra=len(cells.keys()-other.keys()),different_records=len(differing),different_cells=sorted(differing))
        Path(str(args.prefix)+'.paired.json').write_text(json.dumps(result,indent=2))
        print({k:v for k,v in result.items() if not k.endswith('_cells')})
    reference_path=Path(str(args.prefix)+'.reference.bin')
    reference=None
    if reference_path.exists():
        reference={(i%n,i//n%n,i//(n*n)) for i,(owner,) in enumerate(struct.iter_unpack('<I',reference_path.read_bytes()))
                   if owner!=0xffffffff}
    if args.sponza_regions:
        assert reference is not None, 'Regional comparison requires a reference occupancy capture'
        sponza_regions(args.prefix,n,cells,reference)
    for axis in (1,2):
        for fraction in (.32,.4,.5,.6,.68):
            depth=int(n*fraction)
            suffix=f'.axis{axis}-{depth}'
            slice_image(Path(str(args.prefix)+suffix+'.png'),n,cells,axis,depth)
            if reference is not None:
                slice_image(Path(str(args.prefix)+suffix+'.reference-diff.png'),n,cells,axis,depth,reference)


if __name__=='__main__':
    main()
