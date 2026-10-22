#pragma once
#include "AssetLib/ImportErrors.h"
#include "AssetLib/FbxConversions.h"

namespace zen::asset
{
void ValidateFbxDeformers(const ufbx_scene& source, ImportErrors& errors);
// Called by independent mesh workers. All source arrays are immutable.
void ConvertFbxMeshDeformation(const ufbx_mesh&     source,
                               MeshAsset&           mesh,
                               HeapVector<Vertex>&  vertices,
                               MorphPrimitiveAsset& morph,
                               ImportErrors&        errors);
// Called after nodes/meshes are assembled in source order.
void BuildFbxSkins(const ufbx_scene& source, SceneAsset& asset, ImportErrors& errors);
} // namespace zen::asset
