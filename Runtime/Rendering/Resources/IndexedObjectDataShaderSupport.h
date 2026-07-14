#pragma once

#include <string>

#include <Math/Matrix4.h>

#include "Rendering/Data/DrawableObjectDescriptor.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/Resources/ShaderReflection.h"

namespace NLS::Render::Resources
{
inline constexpr uint32_t kIndexedObjectDataPushConstantSize =
    sizeof(NLS::Render::Data::ObjectDrawConstants);
inline constexpr NLS::Render::RHI::ShaderStageMask kIndexedObjectDataPushConstantStageMask =
    NLS::Render::RHI::ShaderStageMask::Vertex |
    NLS::Render::RHI::ShaderStageMask::Fragment;

static_assert(kIndexedObjectDataPushConstantSize == 16u);

enum class IndexedObjectDataShaderStatus : uint8_t
{
    NotIndexed,
    Compatible,
    Incompatible
};

enum class ObjectDrawConstantsStatus : uint8_t
{
    Absent,
    Compatible,
    Incompatible
};

struct ObjectDrawConstantsValidation
{
    ObjectDrawConstantsStatus status = ObjectDrawConstantsStatus::Absent;
    std::string diagnostic;
    NLS::Render::RHI::ShaderStageMask stageMask = NLS::Render::RHI::ShaderStageMask::None;

    [[nodiscard]] bool IsCompatible() const
    {
        return status == ObjectDrawConstantsStatus::Compatible;
    }
};

struct IndexedObjectDataShaderValidation
{
    IndexedObjectDataShaderStatus status = IndexedObjectDataShaderStatus::NotIndexed;
    std::string diagnostic;

    [[nodiscard]] bool IsCompatible() const
    {
        return status == IndexedObjectDataShaderStatus::Compatible;
    }
};

namespace Detail
{
struct ObjectDataDeclarationValidation
{
    bool present = false;
    bool compatible = false;
};

inline ObjectDataDeclarationValidation ValidateParameterStructObjectData(
    const std::vector<ShaderParameterStruct>& parameterStructs)
{
    for (const auto& parameterStruct : parameterStructs)
    {
        for (const auto& member : parameterStruct.members)
        {
            if (member.name != "ObjectData")
                continue;

            const bool compatible =
                parameterStruct.groupKind == ShaderParameterGroupKind::Object &&
                parameterStruct.descriptorSet == NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet &&
                parameterStruct.registerSpace == NLS::Render::RHI::BindingPointMap::kObjectBindingSpace &&
                member.type == NLS::Render::RHI::BindingType::StructuredBuffer &&
                member.binding == 0u &&
                member.count == 1u &&
                member.elementStride == sizeof(NLS::Maths::Matrix4) &&
                NLS::Render::RHI::HasShaderStage(member.stageMask, NLS::Render::RHI::ShaderStageMask::Vertex);
            return { true, compatible };
        }
    }

    return {};
}

inline ObjectDataDeclarationValidation ValidateReflectionObjectData(const ShaderReflection& reflection)
{
    for (const auto& property : reflection.properties)
    {
        if (property.name != "ObjectData")
            continue;

        const bool compatible =
            property.kind == ShaderResourceKind::StructuredBuffer &&
            property.bindingSpace == NLS::Render::RHI::BindingPointMap::kObjectBindingSpace &&
            property.bindingIndex == 0u &&
            property.arraySize == 1 &&
            property.byteSize == sizeof(NLS::Maths::Matrix4) &&
            NLS::Render::RHI::HasShaderStage(property.stageMask, NLS::Render::RHI::ShaderStageMask::Vertex);
        return { true, compatible };
    }

    return {};
}

inline bool HasExpectedObjectDrawConstantMembers(const ShaderConstantBufferDesc& constantBuffer)
{
    if (constantBuffer.members.empty())
        return false;
    if (constantBuffer.members.size() != 4u)
        return false;

    constexpr const char* expectedNames[] = {
        "u_ObjectIndex",
        "u_ObjectFlags",
        "u_ObjectPadding0",
        "u_ObjectPadding1"
    };
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        const auto& member = constantBuffer.members[index];
        if (member.name != expectedNames[index] ||
            member.type != UniformType::UNIFORM_INT ||
            member.byteOffset != index * sizeof(uint32_t) ||
            member.byteSize != sizeof(uint32_t) ||
            member.arraySize != 1u)
        {
            return false;
        }
    }

    return true;
}

inline bool HasExpectedObjectDrawConstantStages(const NLS::Render::RHI::ShaderStageMask stageMask)
{
    const bool hasGraphicsConsumer =
        NLS::Render::RHI::HasShaderStage(stageMask, NLS::Render::RHI::ShaderStageMask::Vertex) ||
        NLS::Render::RHI::HasShaderStage(stageMask, NLS::Render::RHI::ShaderStageMask::Fragment);
    return hasGraphicsConsumer &&
        !NLS::Render::RHI::HasShaderStage(stageMask, NLS::Render::RHI::ShaderStageMask::Compute);
}
}

inline ObjectDrawConstantsValidation ValidateObjectDrawConstants(const Shader& shader)
{
    const auto reflection = shader.GetReflectionSnapshot();
    const ShaderConstantBufferDesc* objectConstants = nullptr;
    for (const auto& constantBuffer : reflection->constantBuffers)
    {
        if (constantBuffer.name != "ObjectIndexConstants")
            continue;
        if (objectConstants != nullptr)
        {
            return {
                ObjectDrawConstantsStatus::Incompatible,
                "ObjectIndexConstants is declared more than once."
            };
        }
        objectConstants = &constantBuffer;
    }

    if (objectConstants == nullptr)
        return {};

    const bool usesObjectDrawBinding =
        objectConstants->bindingSpace == NLS::Render::RHI::BindingPointMap::kObjectBindingSpace &&
        objectConstants->bindingIndex == 1u;
    if (usesObjectDrawBinding && objectConstants->byteSize == sizeof(uint32_t))
        return {};

    if (!usesObjectDrawBinding || objectConstants->byteSize != kIndexedObjectDataPushConstantSize)
    {
        return {
            ObjectDrawConstantsStatus::Incompatible,
            "ObjectIndexConstants must be either the 4-byte b1/space3 legacy descriptor or the 16-byte b1/space3 object-draw ABI."
        };
    }

    if (!Detail::HasExpectedObjectDrawConstantStages(objectConstants->stageMask) ||
        !Detail::HasExpectedObjectDrawConstantMembers(*objectConstants))
    {
        return {
            ObjectDrawConstantsStatus::Incompatible,
            "ObjectIndexConstants must match the 16-byte b1/space3 Vertex and/or Fragment object-draw ABI without Compute visibility."
        };
    }

    return { ObjectDrawConstantsStatus::Compatible, {}, objectConstants->stageMask };
}

inline IndexedObjectDataShaderValidation ValidateIndexedObjectDataShader(const Shader& shader)
{
    const auto parameterStructs = shader.GetParameterStructs();
    const auto reflection = shader.GetReflectionSnapshot();
    const auto objectData = !parameterStructs.empty()
        ? Detail::ValidateParameterStructObjectData(parameterStructs)
        : Detail::ValidateReflectionObjectData(*reflection);

    if (!objectData.present)
        return {};
    if (!objectData.compatible)
    {
        return {
            IndexedObjectDataShaderStatus::Incompatible,
            "ObjectData does not match the indexed object-data ABI."
        };
    }

    const auto objectConstants = ValidateObjectDrawConstants(shader);
    if (objectConstants.status == ObjectDrawConstantsStatus::Absent)
    {
        return {
            IndexedObjectDataShaderStatus::Incompatible,
            "ObjectIndexConstants is required by indexed ObjectData shaders."
        };
    }
    if (objectConstants.status == ObjectDrawConstantsStatus::Incompatible)
    {
        return {
            IndexedObjectDataShaderStatus::Incompatible,
            objectConstants.diagnostic
        };
    }
    if (!NLS::Render::RHI::HasShaderStage(
        objectConstants.stageMask,
        NLS::Render::RHI::ShaderStageMask::Vertex))
    {
        return {
            IndexedObjectDataShaderStatus::Incompatible,
            "ObjectIndexConstants must include Vertex stage for indexed ObjectData shaders."
        };
    }

    return { IndexedObjectDataShaderStatus::Compatible, {} };
}

inline bool ShaderSupportsIndexedObjectData(const Shader& shader)
{
    return ValidateIndexedObjectDataShader(shader).IsCompatible();
}

inline bool BackendSupportsIndexedObjectDataPushConstants(const NLS::Render::RHI::NativeBackendType backend)
{
    return backend == NLS::Render::RHI::NativeBackendType::DX12;
}
}
