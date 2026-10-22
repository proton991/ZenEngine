#pragma once
#include "AssetLib/SceneImporter.h"

namespace zen::asset
{
class FbxSceneImporter final : public ISceneImporter
{
public:
    SceneImportResult Import(const SceneImportRequest& request, const SceneImportContext& context) const override;
};
} // namespace zen::asset
