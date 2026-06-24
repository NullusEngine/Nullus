#include "Rendering/ShaderLab/ShaderLabBindingLayout.h"

#include <algorithm>
#include <utility>

namespace NLS::Render::ShaderLab
{
    namespace
    {
        template <typename TBinding>
        void MergeResourceBinding(
            std::vector<TBinding>& bindings,
            TBinding binding)
        {
            const auto found = std::find_if(
                bindings.begin(),
                bindings.end(),
                [&binding](const TBinding& existing)
                {
                    return existing.name == binding.name &&
                        existing.bindingSpace == binding.bindingSpace &&
                        existing.bindingIndex == binding.bindingIndex;
                });

            if (found != bindings.end())
            {
                found->stageMask = static_cast<NLS::Render::RHI::ShaderStageMask>(
                    static_cast<uint32_t>(found->stageMask) |
                    static_cast<uint32_t>(binding.stageMask));
                found->arraySize = std::max(found->arraySize, binding.arraySize);
                return;
            }

            bindings.push_back(std::move(binding));
        }

        void MergeConstantBufferBinding(
            std::vector<ShaderLabConstantBufferBinding>& bindings,
            ShaderLabConstantBufferBinding binding)
        {
            const auto found = std::find_if(
                bindings.begin(),
                bindings.end(),
                [&binding](const ShaderLabConstantBufferBinding& existing)
                {
                    return existing.name == binding.name &&
                        existing.bindingSpace == binding.bindingSpace &&
                        existing.bindingIndex == binding.bindingIndex;
                });

            if (found != bindings.end())
            {
                found->byteSize = std::max(found->byteSize, binding.byteSize);
                found->stageMask = static_cast<NLS::Render::RHI::ShaderStageMask>(
                    static_cast<uint32_t>(found->stageMask) |
                    static_cast<uint32_t>(binding.stageMask));
                return;
            }

            bindings.push_back(std::move(binding));
        }
    }

    ShaderLabMaterialBindingLayout BuildShaderLabMaterialBindingLayout(
        const NLS::Render::Resources::ShaderReflection& reflection,
        const uint32_t materialBindingSpace,
        const std::string_view materialConstantBufferName)
    {
        ShaderLabMaterialBindingLayout layout;

        for (const auto& cbuffer : reflection.constantBuffers)
        {
            if (cbuffer.bindingSpace != materialBindingSpace)
                continue;
            if (!materialConstantBufferName.empty() && cbuffer.name != materialConstantBufferName)
                continue;

            layout.constantBufferSize = std::max(layout.constantBufferSize, cbuffer.byteSize);
            MergeConstantBufferBinding(layout.constantBuffers, {
                cbuffer.name,
                cbuffer.bindingSpace,
                cbuffer.bindingIndex,
                cbuffer.byteSize,
                cbuffer.stageMask
            });

            for (const auto& member : cbuffer.members)
            {
                const auto found = std::find_if(
                    layout.properties.begin(),
                    layout.properties.end(),
                    [&member](const ShaderLabPropertyBinding& existing)
                    {
                        return existing.name == member.name &&
                            existing.byteOffset == member.byteOffset &&
                            existing.byteSize == member.byteSize;
                    });
                if (found == layout.properties.end())
                {
                    layout.properties.push_back({
                        member.name,
                        member.byteOffset,
                        member.byteSize,
                        member.arraySize
                    });
                }
            }
        }

        for (const auto& property : reflection.properties)
        {
            if (property.bindingSpace != materialBindingSpace)
                continue;

            if (property.kind == NLS::Render::Resources::ShaderResourceKind::SampledTexture)
            {
                MergeResourceBinding(layout.textures, ShaderLabTextureBinding{
                    property.name,
                    property.bindingSpace,
                    property.bindingIndex,
                    property.arraySize,
                    property.stageMask
                });
            }
            else if (property.kind == NLS::Render::Resources::ShaderResourceKind::Sampler)
            {
                MergeResourceBinding(layout.samplers, ShaderLabSamplerBinding{
                    property.name,
                    property.bindingSpace,
                    property.bindingIndex,
                    property.arraySize,
                    property.stageMask
                });
            }
        }

        return layout;
    }
}
