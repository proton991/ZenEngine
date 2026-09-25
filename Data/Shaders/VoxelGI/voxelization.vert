#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/bindless_heap.glsl"
// Keep the full asset::Vertex interface: the RHI derives binding stride from reflection.
layout(location=0) in vec4 inPos;
layout(location=1) in vec4 inNormal;
layout(location=2) in vec4 inTangent;
layout(location=3) in vec2 inUV0;
layout(location=4) in vec2 inUV1;
layout(location=5) in vec4 inJoint0;
layout(location=6) in vec4 inWeight0;
layout(location=7) in vec4 inColor;
struct NodeData { mat4 modelMatrix; mat4 normalMatrix; };
layout(std140,set=3,binding=2) readonly buffer NodeBuffer { NodeData nodesData[]; };
layout(push_constant) uniform Constants { uint nodeIndex; uint materialIndex; uint firstTriangle; uint volumeDimension; } pc;
void main() { gl_Position=nodesData[pc.nodeIndex].modelMatrix*vec4(inPos.xyz,1); }
