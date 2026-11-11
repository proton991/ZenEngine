#ifndef ZEN_RESTIR_GI_H
#define ZEN_RESTIR_GI_H
#define ZEN_RESTIR_RESERVOIR_BYTES 24u
#ifdef __cplusplus
#    include <cstdint>
namespace zen::rc
{
// Scalar words match PackedGIReservoir in restir_reservoir.glsl (std430).
struct PackedGIReservoir
{
    float    x, y, z;
    uint32_t normalCount;
    uint32_t radiance;
    float    weight;
};
static_assert(sizeof(PackedGIReservoir) == ZEN_RESTIR_RESERVOIR_BYTES);
} // namespace zen::rc
#endif
#endif
