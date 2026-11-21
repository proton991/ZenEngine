#ifndef ZEN_HARDWARE_RAY_INTERVAL
#define ZEN_HARDWARE_RAY_INTERVAL
// Query and pipeline traversal share the same finite segment and surface offset.
bool HardwareRayInterval(vec3 origin, vec3 direction, float maximumDistance, out float end)
{
    float enter = 0.0, leave = maximumDistance;
    bool  intersects = true;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(direction[axis]) < 1e-12)
        {
            intersects = intersects && origin[axis] >= rayScene.minimum[axis] && origin[axis] <= rayScene.maximum[axis];
        }
        else
        {
            float a = (rayScene.minimum[axis] - origin[axis]) / direction[axis];
            float b = (rayScene.maximum[axis] - origin[axis]) / direction[axis];
            enter   = max(enter, min(a, b));
            leave   = min(leave, max(a, b));
        }
    }
    end = min(leave + 1e-5, maximumDistance);
    return intersects && leave >= enter && leave > 0.0;
}

#endif
