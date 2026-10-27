#pragma once
#include <memory>
#include "ShaderBindingLayoutUtils.h"

namespace NLS::Render::Resources
{
// Physical layout for one compiled pass/variant/target. Backend descriptors
// remain the existing RHI types and register-space mapping.
struct ShaderBindingLayout
{
    ShaderCompiler::ShaderTargetPlatform target = ShaderCompiler::ShaderTargetPlatform::Unknown;
    uint64_t keywordHash = 0;
    std::shared_ptr<const ShaderReflection> reflection;
    std::vector<RHI::RHIBindingLayoutDesc> sets;
    std::vector<ShaderParameterGroupContract> groups;
};
}
