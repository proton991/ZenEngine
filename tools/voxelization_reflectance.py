"""V1 independent contribution/count oracle; imports V0 geometry only at call time."""
import json
import math
from pathlib import Path
import struct
from voxelization_materials import MaterialOracle, COLOR_TOLERANCE
from voxelization_fixtures import Fixture

SCALE = 4095
ANALYTIC_TOLERANCE = .5/SCALE + .5/255 + 2e-6
# Texture interpolation and barycentric precision retain the pre-existing V0 allowance.
TEXTURED_TOLERANCE = COLOR_TOLERANCE + ANALYTIC_TOLERANCE


def mixture_fixture(folder, dimension, variant):
    fixture = Fixture()
    materials = [dict(pbrMetallicRoughness=dict(baseColorFactor=color, metallicFactor=metal))
                 for color,metal in (([1,0,0,1],0),([0,0,1,1],.5),([0,0,0,1],0),
                                     ([0,1,0,0],0),([1,1,1,1],1))]
    materials[3].update(alphaMode='MASK',alphaCutoff=.5)
    fixture.document['materials'] = materials
    fixture.mesh([[(-.5,)*3]*3,[(.5,)*3]*3])
    def point(x,y,z):
        return (x/dimension-.5,y/dimension-.5,z/dimension-.5)
    triangle = [point(10.125,10.125,10.25),point(10.875,10.125,10.25),point(10.125,10.875,10.25)]
    for material in range(5):
        triangles = [triangle]
        if material == 0 and variant == 'duplicate':
            triangles *= 2
        if material == 0 and variant == 'tessellated':
            a,b,c = triangle
            ab,bc,ca = [tuple((p[i]+q[i])*.5 for i in range(3)) for p,q in ((a,b),(b,c),(c,a))]
            triangles = [[a,ab,ca],[ab,b,bc],[ca,bc,c],[ab,bc,ca]]
        fixture.mesh(triangles,material)
    if variant == 'reverse':
        fixture.document['nodes'].reverse()
    (folder/'material-contract.json').write_text(json.dumps(dict(materials=materials,textures=[])))
    return fixture.save(folder,'mixture')


def compare_reflectance(prefix, metadata, triangles):
    from validate_voxelization import candidate_cells, clip_cell, AMBIGUITY, enabled_records
    n = metadata['resolution']
    assert metadata['reflectance_scale'] == SCALE
    material = MaterialOracle(prefix,metadata,triangles) if (prefix.parent/'material-contract.json').exists() else None
    contributions, ambiguous = {}, set()
    enabled = enabled_records(prefix, metadata)
    for record, triangle in enumerate(triangles):
        if not enabled[record]:
            continue
        for cell in candidate_cells(triangle,n):
            expanded = bool(clip_cell(triangle,cell,AMBIGUITY))
            certain = bool(clip_cell(triangle,cell,-AMBIGUITY))
            if expanded != certain:
                ambiguous.add(cell)
            if not expanded:
                continue
            value = material.surface(record,[x+.5 for x in cell]) if material else dict(
                visible=True,alpha_ambiguous=False,color=[.2,.6,.8,1],metal=.25)
            if value['alpha_ambiguous']:
                ambiguous.add(cell)
            if value['visible'] and clip_cell(triangle,cell):
                rho = [min(1,max(0,c*(1-min(1,max(0,value['metal'])))*.96)) for c in value['color'][:3]]
                contributions.setdefault(cell,[]).append(rho)
    data = Path(str(prefix)+'.reflectance.bin').read_bytes()
    owner_data = Path(str(prefix)+'.voxels.bin').read_bytes()
    assert len(data)==n**3*32
    errors=[]
    measured=max_count=black=mixed=0
    max_mean_error=0.
    for index,(r,g,b,count,packed,lr,lg,lb) in enumerate(struct.iter_unpack('<5I3f',data)):
        cell=(index%n,(index//n)%n,index//(n*n))
        owner=struct.unpack_from('<I',owner_data,index*32)[0]
        assert bool(count)==(owner!=0xffffffff), ('count/occupancy',cell,count,owner)
        assert all(math.isfinite(v) for v in (lr,lg,lb))
        if not count:
            assert not any((r,g,b,packed)), ('uncleared',cell)
        else:
            resolved=[((packed>>(i*8))&255)/255 for i in range(3)]
            expected=[v/(SCALE*count) for v in (r,g,b)]
            assert packed>>24==255 and max(abs(a-b) for a,b in zip(resolved,expected))<=.5/255+1e-6
            max_count=max(max_count,count)
        if cell in ambiguous:
            continue
        samples=contributions.get(cell,[])
        if len(samples)!=count:
            errors.append(dict(cell=cell,count=count,expected=len(samples)))
        elif count:
            measured+=1
            black+=int(any(not any(v) for v in samples))
            mixed+=int(any(v!=samples[0] for v in samples))
            sums=[sum(math.floor(v[i]*SCALE+.5) for v in samples) for i in range(3)]
            # Analytic inputs allow one fixed-point rounding unit per contribution.
            textured=bool(material and material.textures)
            sum_tolerance=count*(math.ceil(COLOR_TOLERANCE*SCALE)+1 if textured else 1)
            mean=[sum(v[i] for v in samples)/count for i in range(3)]
            error=max(abs(a-b) for a,b in zip(resolved,mean))
            max_mean_error=max(max_mean_error,error)
            if max(abs(a-b) for a,b in zip((r,g,b),sums))>sum_tolerance or error>(TEXTURED_TOLERANCE if textured else ANALYTIC_TOLERANCE):
                errors.append(dict(cell=cell,sums=[r,g,b],expected=sums,mean_error=error))
    result=dict(checked_cells=measured,ambiguous_cells=len(ambiguous),max_count=max_count,
        black_contributor_cells=black,mixed_cells=mixed,max_mean_error=max_mean_error,
        analytic_tolerance=ANALYTIC_TOLERANCE,textured_tolerance=TEXTURED_TOLERANCE,
        failures=len(errors),errors=errors[:30])
    Path(str(prefix)+'.reflectance-comparison.json').write_text(json.dumps(result,indent=2))
    return result
