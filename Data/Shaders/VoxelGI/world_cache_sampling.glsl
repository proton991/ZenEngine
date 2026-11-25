#include "environment_sampling.glsl"

vec3 WorldSampleDirection(vec3 normal, vec2 u, bool receiver, out float weight)
{
    // Receiver paths sample cosine; secondary paths mix cosine and uniform for all six
    // records. Both reserve a quarter of the existing rays for an active HDR sky.
    bool  sky      = sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y > 0.0
                  && environmentRows[ZEN_ENVIRONMENT_IMPORTANCE_ROWS] > 0.0;
    float skyShare = sky ? .25 : 0.0;
    vec3  direction;
    if (u.x < skyShare)
    {
        float pdf;
        direction = SampleEnvironment(vec2(u.x / skyShare, u.y), pdf);
    }
    else if (receiver)
    {
        float cosinePdf;
        direction = HybridCosineSample(normal, vec2((u.x - skyShare) / (1.0 - skyShare), u.y), cosinePdf);
    }
    else
    {
        // Inverse CDF of the equal uniform/cosine mixture.
        float z      = sqrt(.25 + 2.0 * (u.x - skyShare) / (1.0 - skyShare)) - .5;
        float radius = sqrt(max(0.0, 1.0 - z * z));
        direction    = HybridBasis(normal) * vec3(radius * cos(2.0 * HYBRID_PI * u.y), radius * sin(2.0 * HYBRID_PI * u.y), z);
    }
    float cosine = max(dot(normal, direction), 0.0);
    float pdfPi  = receiver ? cosine : .25 * (1.0 + 2.0 * cosine);
    if (sky)
    {
        pdfPi = mix(pdfPi, HYBRID_PI * EnvironmentPDF(direction), skyShare);
    }
    weight = 1.0 / max(pdfPi, 1e-20);
    return direction;
}
