#pragma once
#include "RHIResource.h"

namespace zen::rhi
{
class ShaderUtil
{
public:
    static ShaderGroupSPIRVPtr CompileShaderSourceToSPIRV(ShaderGroupSourcePtr shaderGroupSource);

    static void ReflectShaderGroupInfo(ShaderGroupSPIRVPtr shaderGroupSpirv,
                                       ShaderGroupInfo&    shaderGroupInfo);
};


inline ShaderGroupSPIRVPtr ShaderUtil::CompileShaderSourceToSPIRV(
    ShaderGroupSourcePtr shaderGroupSource)
{
    return MakeRefCountPtr<ShaderGroupSPIRV>();
}

inline void ShaderUtil::ReflectShaderGroupInfo(ShaderGroupSPIRVPtr shaderGroupSpirv,
                                               ShaderGroupInfo&    shaderGroupInfo)
{}
} // namespace zen::rhi