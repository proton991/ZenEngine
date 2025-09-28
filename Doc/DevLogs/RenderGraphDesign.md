# RenderGraph Design

Current design only considers 'read-after-write' for resources and the barrier setup is not optimal. This
doc illustrates the new design of `rc::RenderGraph`.

## Types

### `RDGNode`

**attributes**

* `int32_t nodeIndex`
* `int32_t adjNodeIndex`

### `ResourceTracker`

Tracks resource (Texture/Buffer) states, maanged by RenderDevice, used by `RenderGraph`

### `RDGAccess`

### `RenderGraph`

**attributes**:
<br>

* `vector<RDGAccess> m_readAccessList`: All read access in the RDG.
* `vector<RDGAccess> m_writeAccessList`: All write access in the RDG.
* `vector<RDGNode> m_allNodes`: All write access in the RDG.


# Refactor project structure
Move basic definitions in RHICommon to an upper level: such as Rect, Format, Color, SampleCount
RenderDevice calls RHI apis, Renderers call RenderDevice and DO NOT interface with RHI