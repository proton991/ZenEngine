#include "EditorShaders.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"

namespace zen::editor
{
namespace
{
class EditorShader final : public rc::ShaderProgram
{
public:
    EditorShader(rc::RenderDevice& device, NameID name, const char* vertex, const char* fragment) : ShaderProgram(&device, name)
    {
        if (fragment != nullptr)
        {
            AddShaderStage(RHIShaderStage::eVertex, vertex);

            AddShaderStage(RHIShaderStage::eFragment, fragment);
        }
        else
        {
            AddShaderStage(RHIShaderStage::eCompute, vertex);
        }
    }
};
} // namespace

bool RegisterEditorShader(rc::RenderDevice& device, NameID name, const char* vertex, const char* fragment)
{
    rc::ShaderProgramManager& manager = rc::ShaderProgramManager::GetInstance();

    bool valid                        = true;

    if (manager.RequestShaderProgram(name) == nullptr)
    {
        EditorShader* shader = ZEN_NEW() EditorShader(device, name, vertex, fragment);

        valid                = shader->Init();

        if (valid)
        {
            manager.StoreProgram(shader);
        }
        else
        {
            ZEN_DELETE(shader);
        }
    }

    return valid;
}
} // namespace zen::editor
