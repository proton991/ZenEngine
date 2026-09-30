#pragma once

// Keep upstream's packed RGBA ImDrawVert layout and 16-bit indices. The renderer
// supports base-vertex offsets, including draw lists larger than 64K vertices.
// Static atlas mode is deliberate: do not advertise RendererHasTextures until
// create/update/destroy requests and dynamic font uploads are implemented.
