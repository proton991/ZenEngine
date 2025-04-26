#version 450
#extension GL_ARB_shader_image_load_store : require

layout(set = 0, binding = 0, rgba8) uniform readonly image3D voxelRadiance;

layout (push_constant) uniform uNodePushConstant
{
    uint volumeDimension;
} pc;

layout(location = 0) out VS_OUT {
    vec4 albedo;
} vs_out;


void main()
{
    vec3 position = vec3
    (
        (gl_VertexIndex % pc.volumeDimension),
        ((gl_VertexIndex / pc.volumeDimension) % pc.volumeDimension),
        (gl_VertexIndex / (pc.volumeDimension * pc.volumeDimension))
    );

    ivec3 texPos = ivec3(position);

    vs_out.albedo = imageLoad(voxelRadiance, texPos);
    gl_Position = vec4(position, 1.0f);
}