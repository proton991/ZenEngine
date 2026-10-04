#include "../Common/material_surface.glsl"
#include "../Common/material_lighting.glsl"
#include "../Common/scene_lighting.glsl"
#ifdef VOXEL_GI
#include "../VoxelGI/cone_trace.glsl"
#include "../ShadowMapping/scene_shadows.glsl"
#endif
layout(set=1,binding=0) uniform uCameraData { mat4 uProjViewMatrix; mat4 uProjMatrix; mat4 uViewMatrix; };
struct NodeData { mat4 modelMatrix; mat4 normalMatrix; vec4 surfaceScale; };
layout(std140,set=1,binding=1) readonly buffer NodeBuffer { NodeData nodesData[]; };
layout(std140,set=1,binding=2) readonly buffer MaterialBuffer { Material materialData[]; };
layout(set=3,binding=1) uniform samplerCube envIrradianceMap;
layout(push_constant) uniform Constants { uint uNodeIndex; uint uMaterialIndex; uint uTopology; };
layout(location=0) out vec4 outScatterLighting;
layout(location=1) out vec4 outScatterPosition;
void main()
{
    Material material = materialData[uMaterialIndex];
    MaterialSurface s = ReadMaterialSurface(material);
    s.modelScale = nodesData[uNodeIndex].surfaceScale.xyz;
    vec3 N = SurfaceNormal(material);
    if (!MaterialVisible(material, s.baseColor.a) || (uTopology == 2 &&
        (material.materialProperties.z < 0.5 || material.volumeIridescence.x > 0.0) && !(gl_FrontFacing == (inOrientation >= 0.0)))) discard;
    vec3 V = uProjMatrix[3][3] > 0.5 ? normalize(transpose(mat3(uViewMatrix))[2]) : SafeNormalize(sceneUbo.viewPosition.xyz - inWorldPos, N);
    vec3 singleScatter = MultiscatterToSingleScatter(s.multiscatterColor);
    vec3 color = vec3(0.0);
    if (s.diffuseTransmission > 0.0 && max(max(singleScatter.r, singleScatter.g), singleScatter.b) > 0.0)
    {
        vec3 frontIrradiance = texture(envIrradianceMap, EnvironmentDirection(N)).rgb * sceneUbo.environment.x * sceneUbo.environment.z;
        vec3 backIrradiance = texture(envIrradianceMap, EnvironmentDirection(-N)).rgb * sceneUbo.environment.x * sceneUbo.environment.z;
#ifdef VOXEL_GI
        frontIrradiance = DiffuseVoxelLighting(inWorldPos, N);
#endif
        color = (frontIrradiance + backIrradiance * VolumeAttenuation(s, DiffuseVolumeThickness(s)) * (vec3(1.0) - singleScatter)) *
            singleScatter * s.diffuseTransmissionColor * s.diffuseTransmission * (vec3(1.0) - SurfaceFresnel(s, max(dot(N, V), 0.0)));
        for (int i = 0; i <= int(sceneUbo.lightInfo.x); ++i)
        {
            vec3 L;
            float distanceToLight;
            vec3 radiance = EvaluateLight(i == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[i], inWorldPos, L, distanceToLight);
            float nl = dot(N, L);
            vec3 weighting = nl >= 0.0 ? singleScatter : singleScatter * (vec3(1.0) - singleScatter) * VolumeAttenuation(s, DiffuseVolumeThickness(s));
            vec3 mirrorL = nl >= 0.0 ? L : reflect(L, N);
            vec3 H = SafeNormalize(V + mirrorL, N);
            float visibility = 1.0;
#ifdef VOXEL_GI
            visibility = SceneLightVisibility(i == int(sceneUbo.lightInfo.x) ? MAX_SCENE_LIGHTS : i, inWorldPos, GeometricNormal(material));
#endif
            color += radiance * abs(nl) / M_PI * weighting * s.diffuseTransmissionColor * s.diffuseTransmission *
                (vec3(1.0) - SurfaceFresnel(s, max(dot(V, H), 0.0))) * visibility;
        }
        color *= SheenLayerScale(s, max(dot(N, V), 0.0), max(dot(N, V), 0.0));
    }
    outScatterLighting = vec4(color, float(uMaterialIndex + 1));
    outScatterPosition = vec4(inWorldPos, float(uNodeIndex + 1));
}
