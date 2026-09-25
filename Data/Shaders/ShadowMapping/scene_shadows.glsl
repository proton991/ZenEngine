#ifndef ZEN_SCENE_SHADOWS
#define ZEN_SCENE_SHADOWS
#include "../Common/scene_lighting.glsl"
layout(std140,set=4,binding=0) uniform uSceneShadows
{
    mat4 viewProjection[MAX_SCENE_LIGHTS*6];
    vec4 lights[MAX_SCENE_LIGHTS];
    vec4 settings;
} shadows;
layout(set=4,binding=1) uniform sampler2DArray sceneShadowMaps;

int PointShadowFace(vec3 direction)
{
    vec3 a=abs(direction);
    int axis=a.x>=a.y && a.x>=a.z ? 0 : (a.y>=a.z ? 1 : 2);
    return axis*2+(direction[axis]<0 ? 1 : 0);
}

float SceneLightVisibility(int lightIndex,vec3 position,vec3 normal)
{
    vec4 info=shadows.lights[lightIndex];
    float visibility=1.0;
    if(info.y>0)
    {
        SceneLight light=sceneUbo.lights[lightIndex];
        bool directional=int(light.directionType.w)==0;
        vec3 receiver=position+normal*shadows.settings.x;
        int layer=int(info.x);
        vec3 forward=light.directionType.xyz;
        if(info.y==6)
        {
            int face=PointShadowFace(receiver-light.positionRange.xyz);
            layer+=face;
            forward=vec3(0);
            forward[face/2]=(face%2)==0 ? 1.0 : -1.0;
        }
        vec3 helper=abs(forward.y)<0.99 ? vec3(0,1,0) : vec3(0,0,1);
        vec3 right=normalize(cross(forward,helper));
        vec3 up=cross(right,forward);
        vec4 clip=shadows.viewProjection[layer]*vec4(receiver,1);
        vec3 ndc=clip.xyz/max(clip.w,1e-8);
        if(clip.w>0 && all(lessThanEqual(abs(ndc.xy),vec2(1))) && ndc.z>=0 && ndc.z<=1)
        {
            float receiverDepth=directional ? ndc.z : length(receiver-light.positionRange.xyz)*info.z;
            vec2 pixel=(ndc.xy*0.5+0.5)*shadows.settings.y-0.5;
            ivec2 base=ivec2(floor(pixel));
            visibility=0;
            // Separable tent PCF: smooth subtexel transitions, without filtering depth itself.
            for(int y=-1;y<=2;++y)
            for(int x=-1;x<=2;++x)
            {
                ivec2 samplePixel=base+ivec2(x,y);
                vec2 weight=max(vec2(2)-abs(vec2(samplePixel)-pixel),vec2(0));
                samplePixel=clamp(samplePixel,ivec2(0),ivec2(int(shadows.settings.y)-1));
                // Compare against the receiver plane at this tap, not the center depth.
                // This prevents PCF from shadowing a sloped surface with its own neighbors.
                float reference=receiverDepth;
                if(directional)
                {
                    vec2 offset=(vec2(samplePixel)-pixel)*info.w;
                    float denominator=dot(forward,normal);
                    if(abs(denominator)>1e-4)
                        reference-=dot(right*offset.x-up*offset.y,normal)/denominator*info.z;
                }
                else
                {
                    vec2 sampleNDC=(vec2(samplePixel)+0.5)/shadows.settings.y*2.0-1.0;
                    float tangent=info.w*shadows.settings.y*0.5;
                    vec3 ray=forward+(right*sampleNDC.x-up*sampleNDC.y)*tangent;
                    float denominator=dot(ray,normal);
                    if(abs(denominator)>1e-4)
                        reference=dot(receiver-light.positionRange.xyz,normal)/denominator*length(ray)*info.z;
                }
                reference-=shadows.settings.x*info.z;
                float depth=texelFetch(sceneShadowMaps,ivec3(samplePixel,layer),0).r;
                visibility+=weight.x*weight.y*float(reference<=depth)/16.0;
            }
        }
    }
    return visibility;
}
#endif
