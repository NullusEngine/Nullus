#pragma once
#include "MaterialParameterSchema.h"
#include "ShaderBindingLayout.h"

namespace NLS::Render::RHI {class RHIPipelineLayout;}

namespace NLS::Render::Resources
{
// Ownership carried by recorded draws. No authoring graph, source file access,
// mutable parameter map or raw texture pointer enters GPU execution.
struct MaterialRuntimeResource
{
    uint64_t materialInstanceId = 0;
    uint64_t parameterRevision = 0;
    uint64_t renderStateRevision = 0;
    uint64_t shaderInstanceId = 0;
    uint64_t shaderGeneration = 0;
    std::shared_ptr<const MaterialParameterSchema> schema;
    ShaderBindingLayout layout;
    std::shared_ptr<RHI::RHIBindingLayout> materialLayout;
    std::shared_ptr<RHI::RHIBindingSet> materialBindings;
    std::shared_ptr<RHI::RHIPipelineLayout> pipelineLayout;
};
}
