#version 450
layout(location=0) in vec3 sphereNormal;
layout(location=1) flat in vec3 sphereColor;
layout(location=0) out vec4 outFragColor;

void main()
{
    // A colored source indicator; analytic light injection supplies its energy.
    float shade=0.7+0.3*max(dot(normalize(sphereNormal),normalize(vec3(-0.4,0.8,0.5))),0.0);
    vec3 color=clamp(sphereColor*shade,0.0,1.0);
    outFragColor=vec4(pow(color,vec3(1.0/2.2)),1);
}
