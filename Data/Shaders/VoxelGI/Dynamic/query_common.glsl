#include "Graphics/Shared/GIVisibility.h"

GIHit EmptyGIHit(uint status)
{
    return GIHit(vec4(0),vec4(0),vec4(0),vec4(0),vec4(0),uvec4(status,0,GI_INVALID_CELL,0));
}

bool ValidGIQuery(GIQuery ray)
{
    return !any(isnan(ray.originMin)) && !any(isinf(ray.originMin)) &&
        !any(isnan(ray.directionMax)) && !any(isinf(ray.directionMax)) &&
        ray.originMin.w>=0 && ray.directionMax.w>=ray.originMin.w &&
        abs(dot(ray.directionMax.xyz,ray.directionMax.xyz)-1.0)<=2e-4 &&
        ray.source.x>0 && ray.source.x<=GI_ALL &&
        (ray.source.y==0 || ray.source.y==GI_STATIC || ray.source.y==GI_DYNAMIC);
}
