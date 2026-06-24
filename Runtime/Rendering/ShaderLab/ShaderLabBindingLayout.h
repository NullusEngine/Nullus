#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/Resources/ShaderReflection.h"
#include "Rendering/ShaderLab/ShaderLabTypes.h"

namespace NLS::Render::ShaderLab
{
    struct NLS_RENDER_API ShaderLabPropertyBinding
    {
        std::string name;
        uint32_t byteOffset = 0;
        uint32_t byteSize = 0;
        uint32_t arraySize = 1;
    };

    struct NLS_RENDER_API ShaderLabConstantBufferBinding
    {
        std::string name;
        uint32_t bindingSpace = 0;
        uint32_t bindingIndex = 0;
        uint32_t byteSize = 0;
        NLS::Render::RHI::ShaderStageMask stageMask = NLS::Render::RHI::ShaderStageMask::None;
    };

    struct NLS_RENDER_API ShaderLabTextureBinding
    {
        std::string name;
        uint32_t bindingSpace = 0;
        uint32_t bindingIndex = 0;
        int32_t arraySize = 1;
        NLS::Render::RHI::ShaderStageMask stageMask = NLS::Render::RHI::ShaderStageMask::None;
    };

    struct NLS_RENDER_API ShaderLabSamplerBinding
    {
        std::string name;
        uint32_t bindingSpace = 0;
        uint32_t bindingIndex = 0;
        int32_t arraySize = 1;
        NLS::Render::RHI::ShaderStageMask stageMask = NLS::Render::RHI::ShaderStageMask::None;
    };

    struct NLS_RENDER_API ShaderLabMaterialBindingLayout
    {
        uint32_t constantBufferSize = 0;
        std::vector<ShaderLabConstantBufferBinding> constantBuffers;
        std::vector<ShaderLabPropertyBinding> properties;
        std::vector<ShaderLabTextureBinding> textures;
        std::vector<ShaderLabSamplerBinding> samplers;
    };

    NLS_RENDER_API ShaderLabMaterialBindingLayout BuildShaderLabMaterialBindingLayout(
        const NLS::Render::Resources::ShaderReflection& reflection,
        uint32_t materialBindingSpace = NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        std::string_view materialConstantBufferName = "MaterialProperties");
}
