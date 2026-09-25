#version 450
#extension GL_GOOGLE_include_directive : require
// Isolated equivalent of the pinned paper's ORIGINAL-triangle projection.
// Re-express its three orthographic view matrices in canonical grid coordinates.
// This diagnostic profile deliberately has no conservative expansion.
layout(triangles) in;
layout(triangle_strip,max_vertices=3) out;
layout(location=0) flat out uint axis;
layout(location=1) flat out uint triangleRecord;
layout(set=1,binding=0,std140) uniform uVoxelGrid { vec4 gridMinSize; };
layout(push_constant) uniform Constants { uint nodeIndex; uint materialIndex; uint firstTriangle; uint volumeDimension; } pc;
void main()
{
    vec3 n=abs(cross(gl_in[1].gl_Position.xyz-gl_in[0].gl_Position.xyz,
                     gl_in[2].gl_Position.xyz-gl_in[0].gl_Position.xyz));
    if(max(n.x,max(n.y,n.z))==0.0) return;
    uint z=n.x>=n.y && n.x>=n.z ? 0u : (n.y>=n.z ? 1u : 2u);
    for(int i=0;i<3;++i)
    {
        vec3 p=(gl_in[i].gl_Position.xyz-gridMinSize.xyz)/(gridMinSize.w*float(pc.volumeDimension));
        vec3 projected=z==0u ? vec3(p.z,1.0-p.y,p.x) :
                      (z==1u ? vec3(p.z,p.x,p.y) : vec3(1.0-p.x,1.0-p.y,p.z));
        gl_Position=vec4(projected.xy*2.0-1.0,projected.z,1.0);
        axis=z;
        triangleRecord=pc.firstTriangle+uint(gl_PrimitiveIDIn);
        EmitVertex();
    }
    EndPrimitive();
}
