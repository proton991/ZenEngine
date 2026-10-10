#ifndef ZEN_ENVIRONMENT_SAMPLING
#define ZEN_ENVIRONMENT_SAMPLING
#include "environment_distribution.glsl"
#include "hybrid_sampling.glsl"
layout(std430,set=3,binding=13) readonly buffer EnvironmentColumns { float environmentColumns[]; };
layout(std430,set=3,binding=14) readonly buffer EnvironmentRows { float environmentRows[]; };
layout(std430,set=3,binding=15) readonly buffer EnvironmentLuminance { float environmentLuminance[]; };
layout(std430,set=3,binding=16) readonly buffer EnvironmentTiles { float environmentTiles[]; };
layout(std430,set=3,binding=17) readonly buffer EnvironmentSources
{
    uint environmentSourceCount;
    uint environmentSourceTiles[ZEN_ENVIRONMENT_MAX_SOURCES];
    float environmentResidualTiles[ZEN_ENVIRONMENT_TILES + 1u];
};
// The same row and column prefix sums with the source tiles removed: the residual environment is
// drawn exactly like the full one, keeping the sampler's two-dimensional stratification.
layout(std430,set=3,binding=18) readonly buffer EnvironmentResidualColumns { float environmentResidualColumns[]; };
layout(std430,set=3,binding=19) readonly buffer EnvironmentResidualRows { float environmentResidualRows[]; };
float EnvironmentColumn(bool residual, uint i) { return residual ? environmentResidualColumns[i] : environmentColumns[i]; }
float EnvironmentRow(bool residual, uint i) { return residual ? environmentResidualRows[i] : environmentRows[i]; }

// Radiance is a bilinear lookup, so within a texel it ramps toward its neighbours. A texel-uniform
// density leaves that ramp in the estimate, which dominates the variance of sources only a few
// texels wide (a noon sun spans about three). Texels are drawn by the mean of the bilinear
// luminance over their area (environment_columns.comp), and the position within the texel follows
// the same surface: the bilinear interpolation of texel-centre luminances, clamped to the face. On
// each quarter of the texel it is bilinear between nine nodes at local s, t in {0, 1/2, 1}.
struct EnvironmentTexel { float node[9]; float mean; };

EnvironmentTexel EnvironmentTexelNodes(uint row, uint column)
{
    uint face = row / ZEN_ENVIRONMENT_IMPORTANCE_SIZE;
    ivec2 texel = ivec2(column, row % ZEN_ENVIRONMENT_IMPORTANCE_SIZE);
    float l[9];
    for(int dy = -1; dy <= 1; ++dy)
        for(int dx = -1; dx <= 1; ++dx)
        {
            ivec2 q = clamp(texel + ivec2(dx, dy), ivec2(0), ivec2(int(ZEN_ENVIRONMENT_IMPORTANCE_SIZE) - 1));
            l[(dy + 1) * 3 + dx + 1] = environmentLuminance[(face * ZEN_ENVIRONMENT_IMPORTANCE_SIZE + uint(q.y))
                * ZEN_ENVIRONMENT_IMPORTANCE_SIZE + uint(q.x)];
        }
    EnvironmentTexel result;
    vec3 a = vec3(l[0], l[1], l[2]), b = vec3(l[3], l[4], l[5]), c = vec3(l[6], l[7], l[8]);
    for(int j = 0; j < 3; ++j)
    {
        // Node rows t = 0, 1/2, 1 average texel rows (-1, 0), (0) and (0, 1); columns likewise.
        vec3 r = j == 0 ? (a + b) * 0.5 : j == 1 ? b : (b + c) * 0.5;
        result.node[j * 3] = (r.x + r.y) * 0.5;
        result.node[j * 3 + 1] = r.y;
        result.node[j * 3 + 2] = (r.y + r.z) * 0.5;
    }
    float[9] n = result.node;
    result.mean = (n[0] + 2.0 * n[1] + n[2] + 2.0 * n[3] + 4.0 * n[4] + 2.0 * n[5] + n[6] + 2.0 * n[7] + n[8]) / 16.0;
    return result;
}

// Surface value at local (s, t) in [0, 1]^2, divided by its texel mean: the density within the texel.
float EnvironmentTexelDensity(EnvironmentTexel texel, vec2 st)
{
    ivec2 quarter = ivec2(greaterThanEqual(st, vec2(0.5)));
    vec2 f = st * 2.0 - vec2(quarter);
    int i = quarter.y * 3 + quarter.x;
    float value = mix(mix(texel.node[i], texel.node[i + 1], f.x), mix(texel.node[i + 3], texel.node[i + 4], f.x), f.y);
    return texel.mean > 0.0 ? value / texel.mean : 1.0;
}

// Inverse CDF of the density proportional to mix(a, b, x) on [0, 1].
float EnvironmentLinearSample(float u, float a, float b)
{
    return a + b > 0.0 ? u * (a + b) / max(a + sqrt(mix(a * a, b * b, u)), 1e-30) : u;
}

// Inverse CDF of the piecewise-linear density through v0, v1, v2 at x = 0, 1/2, 1.
float EnvironmentPiecewiseSample(float u, float v0, float v1, float v2)
{
    float first = v0 + v1, second = v1 + v2;
    float x = u * (first + second);
    if(first + second <= 0.0)
        return u;
    if(x < first)
        return 0.5 * EnvironmentLinearSample(min(x / first, 1.0), v0, v1);
    return 0.5 + 0.5 * EnvironmentLinearSample(min((x - first) / max(second, 1e-30), 1.0), v1, v2);
}

// Local (s, t) with density EnvironmentTexelDensity, from the marginal in s and the conditional in t.
vec2 EnvironmentTexelSample(EnvironmentTexel texel, vec2 u)
{
    float[9] n = texel.node;
    vec3 marginal = vec3(n[0] + 2.0 * n[3] + n[6], n[1] + 2.0 * n[4] + n[7], n[2] + 2.0 * n[5] + n[8]);
    float s = EnvironmentPiecewiseSample(u.x, marginal.x, marginal.y, marginal.z);
    int i = s < 0.5 ? 0 : 1;
    float f = s * 2.0 - float(i);
    vec3 column = vec3(mix(n[i], n[i + 1], f), mix(n[3 + i], n[4 + i], f), mix(n[6 + i], n[7 + i], f));
    return vec2(s, EnvironmentPiecewiseSample(u.y, column.x, column.y, column.z));
}

// Densities use the stored CDF intervals, including rounding, and the cube's
// area-to-solid-angle Jacobian. Sampling and PDF evaluation share this function.
float EnvironmentCellPDF(uint row, uint column, vec2 uv, bool residual)
{
    uint offset = row * (ZEN_ENVIRONMENT_IMPORTANCE_SIZE + 1u);
    float rowTotal = EnvironmentColumn(residual, offset + ZEN_ENVIRONMENT_IMPORTANCE_SIZE);
    float total = EnvironmentRow(residual, ZEN_ENVIRONMENT_IMPORTANCE_ROWS);
    float probability = (EnvironmentRow(residual, row + 1u) - EnvironmentRow(residual, row))
        * (EnvironmentColumn(residual, offset + column + 1u) - EnvironmentColumn(residual, offset + column));
    return probability / max(total * rowTotal, 1e-30)
        * (float(ZEN_ENVIRONMENT_IMPORTANCE_SIZE * ZEN_ENVIRONMENT_IMPORTANCE_SIZE) * 0.25)
        / EnvironmentCubeJacobian(uv);
}

vec3 EnvironmentWorldDirection(uint face, vec2 uv)
{
    vec3 direction = EnvironmentCubeDirection(face, uv);
    // Invert EnvironmentDirection: inverse authored quaternion, then inverse Y rotation.
    vec4 q = vec4(-sceneUbo.environmentOrientation.xyz, sceneUbo.environmentOrientation.w);
    direction += 2.0 * cross(q.xyz, cross(q.xyz, direction) + q.w * direction);
    float c = cos(sceneUbo.environment.y), s = sin(sceneUbo.environment.y);
    return vec3(c * direction.x + s * direction.z, direction.y, -s * direction.x + c * direction.z);
}

// Face, face coordinates, importance-map texel and position within it of a world direction.
void EnvironmentLocate(vec3 direction, out uint face, out vec2 uv, out uvec2 cell, out vec2 st)
{
    vec3 d = EnvironmentDirection(direction), a = abs(d);
    if(a.x >= a.y && a.x >= a.z)
    {
        face = d.x >= 0.0 ? 0u : 1u;
        uv = vec2(d.x >= 0.0 ? -d.z : d.z, -d.y) / a.x;
    }
    else if(a.y >= a.z)
    {
        face = d.y >= 0.0 ? 2u : 3u;
        uv = vec2(d.x, d.y >= 0.0 ? d.z : -d.z) / a.y;
    }
    else
    {
        face = d.z >= 0.0 ? 4u : 5u;
        uv = vec2(d.z >= 0.0 ? d.x : -d.x, -d.y) / a.z;
    }
    vec2 position = (uv * 0.5 + 0.5) * float(ZEN_ENVIRONMENT_IMPORTANCE_SIZE);
    cell = uvec2(clamp(position, vec2(0), vec2(ZEN_ENVIRONMENT_IMPORTANCE_SIZE - 1u)));
    st = clamp(position - vec2(cell), 0.0, 1.0);
}

float EnvironmentDistributionPDF(vec3 direction, bool residual)
{
    uint face;
    vec2 uv, st;
    uvec2 cell;
    EnvironmentLocate(direction, face, uv, cell, st);
    uint row = face * ZEN_ENVIRONMENT_IMPORTANCE_SIZE + cell.y;
    float pdf = EnvironmentCellPDF(row, cell.x, uv, residual);
    return pdf > 0.0 ? pdf * EnvironmentTexelDensity(EnvironmentTexelNodes(row, cell.x), st) : 0.0;
}

float EnvironmentPDF(vec3 direction) { return EnvironmentDistributionPDF(direction, false); }

vec3 SampleEnvironmentDistribution(vec2 u, bool residual, out float pdf)
{
    float total = EnvironmentRow(residual, ZEN_ENVIRONMENT_IMPORTANCE_ROWS);
    float target = min(u.y * total, uintBitsToFloat(floatBitsToUint(total) - 1u));
    uint low = 0u, high = ZEN_ENVIRONMENT_IMPORTANCE_ROWS;
    while(low + 1u < high)
    {
        uint middle = (low + high) / 2u;
        if(EnvironmentRow(residual, middle) <= target) low = middle;
        else high = middle;
    }
    uint row = low;
    float y = (target - EnvironmentRow(residual, row)) / (EnvironmentRow(residual, row + 1u) - EnvironmentRow(residual, row));
    uint offset = row * (ZEN_ENVIRONMENT_IMPORTANCE_SIZE + 1u);
    total = EnvironmentColumn(residual, offset + ZEN_ENVIRONMENT_IMPORTANCE_SIZE);
    target = min(u.x * total, uintBitsToFloat(floatBitsToUint(total) - 1u));
    low = 0u; high = ZEN_ENVIRONMENT_IMPORTANCE_SIZE;
    while(low + 1u < high)
    {
        uint middle = (low + high) / 2u;
        if(EnvironmentColumn(residual, offset + middle) <= target) low = middle;
        else high = middle;
    }
    float x = (target - EnvironmentColumn(residual, offset + low))
        / (EnvironmentColumn(residual, offset + low + 1u) - EnvironmentColumn(residual, offset + low));
    EnvironmentTexel texel = EnvironmentTexelNodes(row, low);
    vec2 st = min(EnvironmentTexelSample(texel, clamp(vec2(x, y), 0.0, 1.0)), vec2(0.99999994));
    vec2 uv = (vec2(low, row % ZEN_ENVIRONMENT_IMPORTANCE_SIZE) + st) * (2.0 / float(ZEN_ENVIRONMENT_IMPORTANCE_SIZE)) - 1.0;
    pdf = EnvironmentCellPDF(row, low, uv, residual) * EnvironmentTexelDensity(texel, st);
    return EnvironmentWorldDirection(row / ZEN_ENVIRONMENT_IMPORTANCE_SIZE, uv);
}

vec3 SampleEnvironment(vec2 u, out float pdf) { return SampleEnvironmentDistribution(u, false, pdf); }

uint EnvironmentTileOf(uint face, uvec2 cell)
{
    return (face * ZEN_ENVIRONMENT_TILES_PER_FACE + cell.y / ZEN_ENVIRONMENT_TILE_SIZE) * ZEN_ENVIRONMENT_TILES_PER_FACE
        + cell.x / ZEN_ENVIRONMENT_TILE_SIZE;
}

// Density of a position in a texel conditional on its tile: the tile's row interval, the column
// interval within the row segment, the texel-to-solid-angle factor and the bilinear shape.
float EnvironmentTileTexelPDF(uint tile, uint row, uint column, vec2 uv, vec2 st)
{
    uint base = tile * (ZEN_ENVIRONMENT_TILE_SIZE + 1u);
    uint r = row % ZEN_ENVIRONMENT_TILE_SIZE;
    uint offset = row * (ZEN_ENVIRONMENT_IMPORTANCE_SIZE + 1u);
    uint x0 = column - column % ZEN_ENVIRONMENT_TILE_SIZE;
    float mass = environmentTiles[base + ZEN_ENVIRONMENT_TILE_SIZE];
    float segment = environmentColumns[offset + x0 + ZEN_ENVIRONMENT_TILE_SIZE] - environmentColumns[offset + x0];
    if(mass <= 0.0 || segment <= 0.0)
        return 0.0;
    float rowProbability = (environmentTiles[base + r + 1u] - environmentTiles[base + r]) / mass;
    float columnProbability = (environmentColumns[offset + column + 1u] - environmentColumns[offset + column]) / segment;
    return rowProbability * columnProbability * (float(ZEN_ENVIRONMENT_IMPORTANCE_SIZE * ZEN_ENVIRONMENT_IMPORTANCE_SIZE) * 0.25)
        / EnvironmentCubeJacobian(uv) * EnvironmentTexelDensity(EnvironmentTexelNodes(row, column), st);
}

// A direction within one tile, with its density conditional on the tile.
vec3 SampleEnvironmentTile(uint tile, vec2 u, out float pdf)
{
    uint perFace = ZEN_ENVIRONMENT_TILES_PER_FACE * ZEN_ENVIRONMENT_TILES_PER_FACE;
    uint face = tile / perFace, ty = (tile % perFace) / ZEN_ENVIRONMENT_TILES_PER_FACE, tx = tile % ZEN_ENVIRONMENT_TILES_PER_FACE;
    uint base = tile * (ZEN_ENVIRONMENT_TILE_SIZE + 1u);
    float mass = environmentTiles[base + ZEN_ENVIRONMENT_TILE_SIZE];
    float target = min(u.y * mass, uintBitsToFloat(floatBitsToUint(mass) - 1u));
    uint low = 0u, high = ZEN_ENVIRONMENT_TILE_SIZE;
    while(low + 1u < high)
    {
        uint middle = (low + high) / 2u;
        if(environmentTiles[base + middle] <= target) low = middle;
        else high = middle;
    }
    float y = (target - environmentTiles[base + low]) / max(environmentTiles[base + low + 1u] - environmentTiles[base + low], 1e-30);
    uint row = face * ZEN_ENVIRONMENT_IMPORTANCE_SIZE + ty * ZEN_ENVIRONMENT_TILE_SIZE + low;
    uint offset = row * (ZEN_ENVIRONMENT_IMPORTANCE_SIZE + 1u);
    uint x0 = tx * ZEN_ENVIRONMENT_TILE_SIZE;
    float start = environmentColumns[offset + x0];
    float segment = environmentColumns[offset + x0 + ZEN_ENVIRONMENT_TILE_SIZE] - start;
    target = start + min(u.x * segment, uintBitsToFloat(floatBitsToUint(max(segment, 1e-30)) - 1u));
    low = x0; high = x0 + ZEN_ENVIRONMENT_TILE_SIZE;
    while(low + 1u < high)
    {
        uint middle = (low + high) / 2u;
        if(environmentColumns[offset + middle] <= target) low = middle;
        else high = middle;
    }
    float x = (target - environmentColumns[offset + low])
        / max(environmentColumns[offset + low + 1u] - environmentColumns[offset + low], 1e-30);
    EnvironmentTexel texel = EnvironmentTexelNodes(row, low);
    vec2 st = min(EnvironmentTexelSample(texel, clamp(vec2(x, y), 0.0, 1.0)), vec2(0.99999994));
    vec2 uv = (vec2(low, row % ZEN_ENVIRONMENT_IMPORTANCE_SIZE) + st) * (2.0 / float(ZEN_ENVIRONMENT_IMPORTANCE_SIZE)) - 1.0;
    pdf = EnvironmentTileTexelPDF(tile, row, low, uv, st);
    return EnvironmentWorldDirection(face, uv);
}

// The tile of a direction and its density conditional on that tile.
float EnvironmentTilePDF(vec3 direction, out uint tile)
{
    uint face;
    vec2 uv, st;
    uvec2 cell;
    EnvironmentLocate(direction, face, uv, cell, st);
    tile = EnvironmentTileOf(face, cell);
    return EnvironmentTileTexelPDF(tile, face * ZEN_ENVIRONMENT_IMPORTANCE_SIZE + cell.y, cell.x, uv, st);
}

// The residual environment: every tile except the source tiles, drawn by mass.
float EnvironmentResidualTileProbability(uint tile)
{
    float total = environmentResidualTiles[ZEN_ENVIRONMENT_TILES];
    return total > 0.0 ? (environmentResidualTiles[tile + 1u] - environmentResidualTiles[tile]) / total : 0.0;
}

bool EnvironmentSourceTile(uint tile)
{
    bool source = false;
    for(uint k = 0u; k < environmentSourceCount; ++k) source = source || environmentSourceTiles[k] == tile;
    return source;
}

// Density of the residual proposal, and whether the direction lies in a source tile.
float EnvironmentResidualPDF(vec3 direction, out bool source)
{
    uint face;
    vec2 uv, st;
    uvec2 cell;
    EnvironmentLocate(direction, face, uv, cell, st);
    source = EnvironmentSourceTile(EnvironmentTileOf(face, cell));
    return source ? 0.0 : EnvironmentDistributionPDF(direction, true);
}

// Share of sky samples drawn from the environment distribution; the rest are cosine
// samples. Callers allocate exactly this share (or select each proposal with this
// probability), so the mixture density below is the sampling density. Indoor receivers
// see the sky through openings, and environment samples find its bright parts there;
// the cosine share bounds the weight of dim visible directions (2026-10-09 measurements
// in Doc/HybridGI/P4.md).
const float HYBRID_ENVIRONMENT_SHARE = 0.75;

vec3 HybridSkySample(vec3 normal, vec2 u, bool environmentSample, out float pdf)
{
    vec3 direction;
    if(environmentRows[ZEN_ENVIRONMENT_IMPORTANCE_ROWS] <= 0.0)
        direction = HybridCosineSample(normal, u, pdf);
    else
    {
        if(environmentSample)
        {
            direction = SampleEnvironment(u, pdf);
            pdf = HYBRID_ENVIRONMENT_SHARE * pdf + (1.0 - HYBRID_ENVIRONMENT_SHARE) * max(dot(normal, direction), 0.0) / HYBRID_PI;
        }
        else
        {
            direction = HybridCosineSample(normal, u, pdf);
            pdf = (1.0 - HYBRID_ENVIRONMENT_SHARE) * pdf + HYBRID_ENVIRONMENT_SHARE * EnvironmentPDF(direction);
        }
    }
    return direction;
}

// HybridSkySample over the residual environment. Source tiles are integrated by their own rays;
// a cosine sample that lands in one reports it and contributes nothing to the residual.
vec3 HybridResidualSample(vec3 normal, vec2 u, bool environmentSample, out float pdf, out bool source)
{
    vec3 direction;
    if(environmentResidualRows[ZEN_ENVIRONMENT_IMPORTANCE_ROWS] <= 0.0)
    {
        direction = HybridCosineSample(normal, u, pdf);
        EnvironmentResidualPDF(direction, source);
    }
    else if(environmentSample)
    {
        direction = SampleEnvironmentDistribution(u, true, pdf);
        source = false;
        pdf = HYBRID_ENVIRONMENT_SHARE * pdf + (1.0 - HYBRID_ENVIRONMENT_SHARE) * max(dot(normal, direction), 0.0) / HYBRID_PI;
    }
    else
    {
        direction = HybridCosineSample(normal, u, pdf);
        pdf = (1.0 - HYBRID_ENVIRONMENT_SHARE) * pdf + HYBRID_ENVIRONMENT_SHARE * EnvironmentResidualPDF(direction, source);
    }
    return direction;
}

// Proposal for sample i of count: a deterministic split when count is a multiple of four,
// otherwise a stratified per-sample choice that consumes and remaps u.x.
bool HybridEnvironmentProposal(uint i, uint count, inout vec2 u)
{
    if(count % 4u == 0u)
        return i < count * 3u / 4u;
    bool environment = u.x < HYBRID_ENVIRONMENT_SHARE;
    u.x = environment ? u.x / HYBRID_ENVIRONMENT_SHARE : (u.x - HYBRID_ENVIRONMENT_SHARE) / (1.0 - HYBRID_ENVIRONMENT_SHARE);
    u.x = min(u.x, 0.99999994);
    return environment;
}
#endif
