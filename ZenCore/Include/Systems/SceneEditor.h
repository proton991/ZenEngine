#pragma once

namespace zen::sg
{
class Scene;
}

namespace zen::sys
{
class SceneEditor
{
public:
    static void CenterAndNormalizeScene(sg::Scene* scene);
};
} // namespace zen::sys