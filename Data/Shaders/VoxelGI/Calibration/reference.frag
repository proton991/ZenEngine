#version 450
// The pinned reference maps gl_FragCoord to one cell per fragment and has no
// alpha-cutoff test. Capture occupancy only; do not reproduce its spin-lock color average.
layout(location=0) flat in uint axis;
layout(location=1) flat in uint triangleRecord;
layout(location=0) out vec4 color;
layout(set=4,binding=0,r32ui) uniform uimage3D voxelOwner;
void main()
{
    int n=imageSize(voxelOwner).x;
    ivec3 p=ivec3(gl_FragCoord.x,float(n)-gl_FragCoord.y,min(float(n)*gl_FragCoord.z,float(n-1)));
    ivec3 cell=axis==0u ? p.zyx : (axis==1u ? ivec3(n-1-p.y,p.z,p.x) : ivec3(n-1-p.x,p.y,p.z));
    if(all(greaterThanEqual(cell,ivec3(0))) && all(lessThan(cell,ivec3(n))))
        imageAtomicMin(voxelOwner,cell,triangleRecord);
    color=vec4(1,0,0,1);
}
