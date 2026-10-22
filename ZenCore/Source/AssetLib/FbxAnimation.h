#pragma once
#include "AssetLib/ImportErrors.h"
#include "AssetLib/FbxConversions.h"

namespace zen::asset
{
// Shared by initial material conversion and evaluated animation samples.
void SetFbxMaterialFactors(const ufbx_material& source, MaterialAsset& material);
void BakeFbxAnimations(const ufbx_scene&                  source,
                       SceneAsset&                        asset,
                       const AnimationBakeSettings&       settings,
                       ImportErrors&                      errors,
                       HeapVector<SceneImportDiagnostic>& diagnostics);
} // namespace zen::asset
