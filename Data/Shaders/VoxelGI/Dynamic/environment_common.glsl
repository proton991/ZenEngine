layout(set=3,binding=13) uniform samplerCube environmentMap;
vec3 GIEnvironmentRadiance(vec3 direction) {
    return textureLod(environmentMap,EnvironmentSourceDirection(direction),0).rgb *
        (sceneUbo.environment.x*sceneUbo.environment.z);
}
