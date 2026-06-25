#pragma once

#include <Math/Matrix4.h>

#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/Resources/ShaderReflection.h"

namespace NLS::Render::Resources
{
inline bool ShaderSupportsIndexedObjectData(const Shader& shader)
{
    const auto parameterStructs = shader.GetParameterStructs();
    for (const auto& parameterStruct : parameterStructs)
    {
        if (parameterStruct.groupKind != ShaderParameterGroupKind::Object ||
            parameterStruct.descriptorSet != NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet ||
            parameterStruct.registerSpace != NLS::Render::RHI::BindingPointMap::kObjectBindingSpace)
        {
            continue;
        }

        for (const auto& member : parameterStruct.members)
        {
            if (member.name == "ObjectData" &&
                member.type == NLS::Render::RHI::BindingType::StructuredBuffer &&
                member.binding == 0u &&
                member.count == 1u &&
                member.elementStride == sizeof(NLS::Maths::Matrix4) &&
                NLS::Render::RHI::HasShaderStage(member.stageMask, NLS::Render::RHI::ShaderStageMask::Vertex))
            {
                return true;
            }
        }
    }

    if (!parameterStructs.empty())
        return false;

    const auto reflection = shader.GetReflectionSnapshot();
    for (const auto& property : reflection->properties)
    {
        if (property.name == "ObjectData" &&
            property.kind == ShaderResourceKind::StructuredBuffer &&
            property.bindingSpace == NLS::Render::RHI::BindingPointMap::kObjectBindingSpace &&
            property.bindingIndex == 0u &&
            property.arraySize == 1 &&
            property.byteSize == sizeof(NLS::Maths::Matrix4) &&
            NLS::Render::RHI::HasShaderStage(property.stageMask, NLS::Render::RHI::ShaderStageMask::Vertex))
        {
            return true;
        }
    }

    return false;
}

inline bool BackendSupportsIndexedObjectDataPushConstants(const NLS::Render::RHI::NativeBackendType backend)
{
    return backend == NLS::Render::RHI::NativeBackendType::DX12;
}
}
