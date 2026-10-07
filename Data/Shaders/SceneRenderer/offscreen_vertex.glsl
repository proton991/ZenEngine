
#include "../Common/bindless_heap.glsl"
#include "../Common/material.glsl"

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inNormal;
layout (location = 2) in vec4 inTangent;
layout (location = 3) in vec2 inUV0;
layout (location = 4) in vec2 inUV1;
layout (location = 5) in vec4 inJoint0;
layout (location = 6) in vec4 inWeight0;
layout (location = 7) in vec4 inColor;

layout(set = 1, binding = 0) uniform uCameraData
{
    mat4 uProjViewMatrix;
    mat4 uProjMatrix;
    mat4 uViewMatrix;
};

// scene graph node data
struct NodeData {
    mat4 modelMatrix;
    mat4 normalMatrix;
    vec4 surfaceScale;
};

layout(std140, set = 1, binding = 1) readonly buffer NodeBuffer {
    NodeData nodesData[];
};
layout(std140, set = 1, binding = 2) readonly buffer MaterialBuffer { Material materialData[]; };
layout(std430, set = 1, binding = 3) readonly buffer UVBuffer { vec4 uvValues[]; };

layout (push_constant) uniform uNodePushConstant
{
    uint uNodeIndex;
    uint uMaterialIndex;
#ifdef HYBRID_RECEIVER
    uvec2 reserved;
    mat4 previousModel;
#endif
};

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec4 outColor;
layout (location = 2) out vec3 outWorldPos;
layout (location = 3) out vec4 outTangent;
layout (location = 4) flat out float outOrientation;
layout (location = 5) out vec4 outPackedBindingUV[11];
out gl_PerVertex { vec4 gl_Position; float gl_PointSize; };

#ifdef HYBRID_RECEIVER
layout(set=1,binding=4) uniform uPreviousCamera { mat4 previousProjectionView; };
layout(location=16) out vec4 outPreviousClip;
#endif
void main()
{
    vec4 locPos = nodesData[uNodeIndex].modelMatrix * vec4(inPos.xyz, 1.0);

    gl_Position = uProjViewMatrix * vec4(locPos.xyz, 1.0);
    gl_PointSize = 1.0;
#ifdef HYBRID_RECEIVER
    outPreviousClip = previousProjectionView * previousModel * vec4(inPos.xyz,1.0);
#endif

    // Vertex position in world space
    outWorldPos = locPos.xyz / locPos.w;

    // Normal in world space
    mat3 mNormal = mat3(nodesData[uNodeIndex].normalMatrix);
    outNormal = mNormal * inNormal.xyz;
    mat3 mModel = mat3(nodesData[uNodeIndex].modelMatrix);
    outTangent = vec4(mModel * inTangent.xyz, inTangent.w * sign(determinant(mModel)));

    outOrientation = nodesData[uNodeIndex].surfaceScale.w;

    // Currently just vertex color
    outColor = inColor;
    Material material = materialData[uMaterialIndex];
    int sets[5] = int[5](material.bcTexSet, material.mrTexSet, material.normalTexSet,
                        material.aoTexSet, material.emissiveTexSet);
    uint stride = uint(uvValues[0].x);
    for (int slot = 0; slot < 22; ++slot)
    {
        uint set = uint(max(slot < 5 ? sets[slot] : int(material.featureTextures[slot - 5].properties.y), 0));
        vec2 coordinates = stride > set ? uvValues[1 + uint(gl_VertexIndex) * stride + set].xy : vec2(0.0);
        if ((slot & 1) == 0) outPackedBindingUV[slot / 2].xy = coordinates;
        else outPackedBindingUV[slot / 2].zw = coordinates;
    }
}
