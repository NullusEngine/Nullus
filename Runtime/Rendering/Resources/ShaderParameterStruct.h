#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/Data/DrawableObjectDescriptor.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/Resources/ShaderBindingLayoutUtils.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
    class Shader;

    struct ShaderParameterMember
    {
        std::string name;
        RHI::BindingType type = RHI::BindingType::UniformBuffer;
        uint32_t binding = 0u;
        uint32_t count = 1u;
        uint32_t byteSize = 0u;
        uint32_t elementStride = 0u;
        RHI::ShaderStageMask stageMask = RHI::ShaderStageMask::Compute;
        bool required = true;
    };

    struct ShaderParameterStruct
    {
        std::string debugName;
        ShaderParameterGroupKind groupKind = ShaderParameterGroupKind::Pass;
        uint32_t descriptorSet = RHI::BindingPointMap::kPassDescriptorSet;
        uint32_t registerSpace = RHI::BindingPointMap::kPassBindingSpace;
        std::vector<ShaderParameterMember> members;
    };

    struct GlobalShader
    {
        std::string debugName;
        ShaderCompiler::ShaderStage stage = ShaderCompiler::ShaderStage::Compute;
        Shader* shader = nullptr;
        ShaderParameterStruct parameters;
    };

    struct ShaderParameterBindingValue
    {
        std::string name;
        RHI::BindingType type = RHI::BindingType::UniformBuffer;
        std::shared_ptr<RHI::RHIBuffer> buffer;
        uint64_t bufferOffset = 0u;
        uint64_t bufferRange = 0u;
        uint32_t elementStride = 0u;
        std::shared_ptr<RHI::RHITextureView> textureView;
        std::shared_ptr<RHI::RHISampler> sampler;

        static ShaderParameterBindingValue UniformBuffer(
            std::string name,
            std::shared_ptr<RHI::RHIBuffer> buffer,
            const uint64_t range,
            const uint64_t offset = 0u)
        {
            return {
                std::move(name),
                RHI::BindingType::UniformBuffer,
                std::move(buffer),
                offset,
                range,
                0u,
                nullptr,
                nullptr
            };
        }

        static ShaderParameterBindingValue StructuredBuffer(
            std::string name,
            std::shared_ptr<RHI::RHIBuffer> buffer,
            const uint64_t range,
            const uint64_t offset = 0u,
            const uint32_t elementStride = sizeof(uint32_t))
        {
            return {
                std::move(name),
                RHI::BindingType::StructuredBuffer,
                std::move(buffer),
                offset,
                range,
                elementStride,
                nullptr,
                nullptr
            };
        }

        static ShaderParameterBindingValue StorageBuffer(
            std::string name,
            std::shared_ptr<RHI::RHIBuffer> buffer,
            const uint64_t range,
            const uint64_t offset = 0u,
            const uint32_t elementStride = sizeof(uint32_t))
        {
            return {
                std::move(name),
                RHI::BindingType::StorageBuffer,
                std::move(buffer),
                offset,
                range,
                elementStride,
                nullptr,
                nullptr
            };
        }

        static ShaderParameterBindingValue Texture(
            std::string name,
            std::shared_ptr<RHI::RHITextureView> textureView)
        {
            return {
                std::move(name),
                RHI::BindingType::Texture,
                nullptr,
                0u,
                0u,
                0u,
                std::move(textureView),
                nullptr
            };
        }

        static ShaderParameterBindingValue RWTexture(
            std::string name,
            std::shared_ptr<RHI::RHITextureView> textureView)
        {
            return {
                std::move(name),
                RHI::BindingType::RWTexture,
                nullptr,
                0u,
                0u,
                0u,
                std::move(textureView),
                nullptr
            };
        }
    };

    class ShaderParameterStructBuilder
    {
    public:
        explicit ShaderParameterStructBuilder(std::string debugName)
        {
            m_struct.debugName = std::move(debugName);
            SetGroup(ShaderParameterGroupKind::Pass);
        }

        ShaderParameterStructBuilder& SetGroup(const ShaderParameterGroupKind groupKind)
        {
            m_struct.groupKind = groupKind;
            switch (groupKind)
            {
            case ShaderParameterGroupKind::Frame:
                m_struct.registerSpace = RHI::BindingPointMap::kFrameBindingSpace;
                break;
            case ShaderParameterGroupKind::Material:
                m_struct.registerSpace = RHI::BindingPointMap::kMaterialBindingSpace;
                break;
            case ShaderParameterGroupKind::Object:
                m_struct.registerSpace = RHI::BindingPointMap::kObjectBindingSpace;
                break;
            case ShaderParameterGroupKind::Pass:
            default:
                m_struct.registerSpace = RHI::BindingPointMap::kPassBindingSpace;
                break;
            }
            m_struct.descriptorSet = RHI::BindingPointMap::GetDescriptorSetIndex(m_struct.registerSpace);
            return *this;
        }

        ShaderParameterStructBuilder& AddUniformBuffer(
            std::string name,
            const uint32_t binding,
            const uint32_t byteSize,
            const RHI::ShaderStageMask stageMask)
        {
            return AddMember(std::move(name), RHI::BindingType::UniformBuffer, binding, byteSize, stageMask);
        }

        ShaderParameterStructBuilder& AddStructuredBuffer(
            std::string name,
            const uint32_t binding,
            const RHI::ShaderStageMask stageMask,
            const uint32_t elementStride = sizeof(uint32_t))
        {
            return AddMember(std::move(name), RHI::BindingType::StructuredBuffer, binding, 0u, stageMask, elementStride);
        }

        ShaderParameterStructBuilder& AddStorageBuffer(
            std::string name,
            const uint32_t binding,
            const RHI::ShaderStageMask stageMask,
            const uint32_t elementStride = sizeof(uint32_t))
        {
            return AddMember(std::move(name), RHI::BindingType::StorageBuffer, binding, 0u, stageMask, elementStride);
        }

        ShaderParameterStructBuilder& AddTexture(
            std::string name,
            const uint32_t binding,
            const RHI::ShaderStageMask stageMask)
        {
            return AddMember(std::move(name), RHI::BindingType::Texture, binding, 0u, stageMask);
        }

        ShaderParameterStructBuilder& AddRWTexture(
            std::string name,
            const uint32_t binding,
            const RHI::ShaderStageMask stageMask)
        {
            return AddMember(std::move(name), RHI::BindingType::RWTexture, binding, 0u, stageMask);
        }

        ShaderParameterStructBuilder& AddSampler(
            std::string name,
            const uint32_t binding,
            const RHI::ShaderStageMask stageMask)
        {
            return AddMember(std::move(name), RHI::BindingType::Sampler, binding, 0u, stageMask);
        }

        [[nodiscard]] ShaderParameterStruct Build() const
        {
            return m_struct;
        }

    private:
        ShaderParameterStructBuilder& AddMember(
            std::string name,
            const RHI::BindingType type,
            const uint32_t binding,
            const uint32_t byteSize,
            const RHI::ShaderStageMask stageMask,
            const uint32_t elementStride = 0u)
        {
            m_struct.members.push_back({
                std::move(name),
                type,
                binding,
                1u,
                byteSize,
                elementStride,
                stageMask,
                true
            });
            return *this;
        }

        ShaderParameterStruct m_struct;
    };

    inline RHI::RHIBindingLayoutDesc BuildBindingLayoutDescFromShaderParameters(
        const ShaderParameterStruct& parameters,
        std::string_view debugName = {})
    {
        auto isRendererOwnedObjectIndexConstant = [&parameters](const ShaderParameterMember& member)
        {
            return parameters.groupKind == ShaderParameterGroupKind::Object &&
                parameters.registerSpace == RHI::BindingPointMap::kObjectBindingSpace &&
                member.name == "ObjectIndexConstants" &&
                member.type == RHI::BindingType::UniformBuffer &&
                member.binding == 1u &&
                member.byteSize == sizeof(NLS::Render::Data::ObjectDrawConstants);
        };

        RHI::RHIBindingLayoutDesc desc;
        desc.debugName = debugName.empty()
            ? parameters.debugName + "BindingLayout"
            : std::string(debugName);
        desc.entries.reserve(parameters.members.size());
        for (const auto& member : parameters.members)
        {
            if (isRendererOwnedObjectIndexConstant(member))
                continue;

            desc.entries.push_back({
                member.name,
                member.type,
                parameters.descriptorSet,
                member.binding,
                member.count,
                member.stageMask,
                parameters.registerSpace,
                member.elementStride
            });
        }
        return desc;
    }

    inline std::vector<RHI::RHIBindingLayoutDesc> BuildBindingLayoutDescsFromShaderParameters(
        const std::vector<ShaderParameterStruct>& parameterStructs,
        std::string_view debugNamePrefix = {})
    {
        uint32_t maxSetIndex = 0u;
        bool hasAnyParameters = false;
        for (const auto& parameters : parameterStructs)
        {
            maxSetIndex = (std::max)(maxSetIndex, parameters.descriptorSet);
            hasAnyParameters = true;
        }

        if (!hasAnyParameters)
            return {};

        std::vector<RHI::RHIBindingLayoutDesc> layoutDescs(maxSetIndex + 1u);
        for (uint32_t setIndex = 0u; setIndex < layoutDescs.size(); ++setIndex)
        {
            layoutDescs[setIndex].debugName = debugNamePrefix.empty()
                ? "ShaderParametersSet" + std::to_string(setIndex) + "BindingLayout"
                : std::string(debugNamePrefix) + ":Set" + std::to_string(setIndex) + "BindingLayout";
        }

        for (const auto& parameters : parameterStructs)
        {
            auto& layoutDesc = layoutDescs[parameters.descriptorSet];
            if (!debugNamePrefix.empty())
                layoutDesc.debugName = std::string(debugNamePrefix) + ":" + parameters.debugName + "BindingLayout";
            for (const auto& member : parameters.members)
            {
                if (parameters.groupKind == ShaderParameterGroupKind::Object &&
                    parameters.registerSpace == RHI::BindingPointMap::kObjectBindingSpace &&
                    member.name == "ObjectIndexConstants" &&
                    member.type == RHI::BindingType::UniformBuffer &&
                    member.binding == 1u &&
                    member.byteSize == sizeof(NLS::Render::Data::ObjectDrawConstants))
                {
                    continue;
                }

                layoutDesc.entries.push_back({
                    member.name,
                    member.type,
                    parameters.descriptorSet,
                    member.binding,
                    member.count,
                    member.stageMask,
                    parameters.registerSpace,
                    member.elementStride
                });
            }
        }

        return layoutDescs;
    }

    inline RHI::RHIBindingSetDesc BuildBindingSetDescFromShaderParameters(
        const ShaderParameterStruct& parameters,
        std::shared_ptr<RHI::RHIBindingLayout> layout,
        const std::vector<ShaderParameterBindingValue>& values,
        std::string_view debugName)
    {
        RHI::RHIBindingSetDesc desc;
        desc.layout = std::move(layout);
        desc.debugName = std::string(debugName);
        desc.entries.reserve(parameters.members.size());

        for (const auto& member : parameters.members)
        {
            const auto found = std::find_if(
                values.begin(),
                values.end(),
                [&member](const ShaderParameterBindingValue& value)
                {
                    return value.name == member.name && value.type == member.type;
                });
            if (found == values.end())
                continue;

            desc.entries.push_back({
                member.binding,
                found->type,
                found->buffer,
                found->bufferOffset,
                found->bufferRange,
                found->elementStride,
                found->textureView,
                found->sampler
            });
        }

        return desc;
    }
}
