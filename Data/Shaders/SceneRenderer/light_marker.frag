#version 450
layout(location=0) flat in vec3 markerColor;
layout(location=1) in vec3 boxPosition;
layout(location=0) out vec4 outFragColor;

void main()
{
    // Match the deferred pass's display transform while keeping the light's color.
    vec3 color = markerColor / (markerColor + vec3(1.0));
    color = pow(max(color,vec3(0.0)),vec3(1.0/2.2));
    // A screen-space-width edge keeps white light sources legible against bright walls.
    // On a box face one distance is zero; the median is the distance to its nearest edge.
    vec3 distances = max(vec3(0.5) - abs(boxPosition),vec3(0.0));
    float edgeDistance = min(max(distances.x,distances.y),
                             min(max(distances.y,distances.z),max(distances.z,distances.x)));
    float pixelWidth = max(fwidth(edgeDistance),1e-6);
    float interior = smoothstep(0.5*pixelWidth,1.5*pixelWidth,edgeDistance);
    outFragColor = vec4(mix(vec3(0.035),color,interior),1.0);
}
