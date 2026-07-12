#include "Rendering/Resources/MaterialVariantKey.h"

#include <sstream>

#include "Rendering/Resources/Shader.h"

namespace NLS::Render::Resources
{
    namespace
    {
        void AppendBool(std::string& key, std::string_view name, bool value)
        {
            key += "|";
            key += name;
            key += value ? ":1" : ":0";
        }

        void AppendOptionalBool(std::string& key, std::string_view name, const std::optional<bool>& value)
        {
            key += "|";
            key += name;
            key += ":";
            key += value.has_value() ? (*value ? "1" : "0") : "unset";
        }

        void AppendOptionalUInt(std::string& key, std::string_view name, const std::optional<uint32_t>& value)
        {
            key += "|";
            key += name;
            key += ":";
            key += value.has_value()
                ? std::to_string(*value)
                : std::string("unset");
        }

        void AppendOptionalCullFace(
            std::string& key,
            std::string_view name,
            const std::optional<NLS::Render::Settings::ECullFace>& value)
        {
            key += "|";
            key += name;
            key += ":";
            key += value.has_value()
                ? std::to_string(static_cast<uint32_t>(*value))
                : std::string("unset");
        }

        void AppendOptionalCompare(
            std::string& key,
            std::string_view name,
            const std::optional<NLS::Render::Settings::EComparaisonAlgorithm>& value)
        {
            key += "|";
            key += name;
            key += ":";
            key += value.has_value()
                ? std::to_string(static_cast<uint32_t>(*value))
                : std::string("unset");
        }

        void AppendOptionalTextureFormat(
            std::string& key,
            std::string_view name,
            const std::optional<NLS::Render::RHI::TextureFormat>& value)
        {
            key += "|";
            key += name;
            key += ":";
            key += value.has_value()
                ? std::to_string(static_cast<uint32_t>(*value))
                : std::string("unset");
        }

        void AppendOptionalColorFormats(
            std::string& key,
            const bool hasOverride,
            const std::span<const NLS::Render::RHI::TextureFormat> formats)
        {
            key += "|overrideColorFormats:";
            if (!hasOverride)
            {
                key += "unset";
                return;
            }

            key += std::to_string(formats.size());
            for (const auto format : formats)
            {
                key += ",";
                key += std::to_string(static_cast<uint32_t>(format));
            }
        }

        void AppendOptionalColorSpaces(
            std::string& key,
            const bool hasOverride,
            const std::span<const NLS::Render::RHI::TextureColorSpace> spaces)
        {
            key += "|overrideColorSpaces:";
            if (!hasOverride)
            {
                key += "unset";
                return;
            }

            key += std::to_string(spaces.size());
            for (const auto space : spaces)
            {
                key += ",";
                key += std::to_string(static_cast<uint32_t>(space));
            }
        }

        void AppendOptionalRenderTargetBlendStates(
            std::string& key,
            const bool hasOverride,
            const std::span<const NLS::Render::RHI::RHIRenderTargetBlendStateDesc> states)
        {
            key += "|overrideRenderTargetBlendStates:";
            if (!hasOverride)
            {
                key += "unset";
                return;
            }

            key += std::to_string(states.size());
            for (const auto& state : states)
            {
                key += ",";
                key += state.blendEnable ? "1" : "0";
                key += ":";
                key += std::to_string(static_cast<uint32_t>(state.srcColor));
                key += ":";
                key += std::to_string(static_cast<uint32_t>(state.dstColor));
                key += ":";
                key += std::to_string(static_cast<uint32_t>(state.colorOp));
                key += ":";
                key += std::to_string(static_cast<uint32_t>(state.srcAlpha));
                key += ":";
                key += std::to_string(static_cast<uint32_t>(state.dstAlpha));
                key += ":";
                key += std::to_string(static_cast<uint32_t>(state.alphaOp));
                key += ":";
                key += std::to_string(static_cast<uint32_t>(state.colorWriteMask));
            }
        }

        std::string BuildPipelineStateKey(const Data::PipelineState& pipelineState)
        {
            std::string key = "pipeline:";
            key += "depthFunc:" + std::to_string(static_cast<uint32_t>(pipelineState.depthFunc));
            AppendBool(key, "depthWrite", pipelineState.depthWriting);
            AppendBool(key, "depthTest", pipelineState.depthTest);
            AppendBool(key, "blend", pipelineState.blending);
            AppendBool(key, "alphaToCoverage", pipelineState.sampleAlphaToCoverage);
            AppendBool(key, "multisample", pipelineState.multisample);
            key += "|colorMask:" + std::to_string(static_cast<uint32_t>(pipelineState.colorWriting.mask));
            AppendBool(key, "culling", pipelineState.culling);
            key += "|cullFace:" + std::to_string(static_cast<uint32_t>(pipelineState.cullFace));
            key += "|raster:" + std::to_string(static_cast<uint32_t>(pipelineState.rasterizationMode));
            AppendBool(key, "stencil", pipelineState.stencilTest);
            key += "|stencilRef:" + std::to_string(static_cast<uint32_t>(pipelineState.stencilFuncRef));
            key += "|stencilRead:" + std::to_string(static_cast<uint32_t>(pipelineState.stencilFuncMask));
            key += "|stencilWrite:" + std::to_string(static_cast<uint32_t>(pipelineState.stencilWriteMask));
            key += "|stencilCompare:" + std::to_string(static_cast<uint32_t>(pipelineState.stencilFuncOp));
            key += "|stencilFail:" + std::to_string(static_cast<uint32_t>(pipelineState.stencilOpFail));
            key += "|stencilDepthFail:" + std::to_string(static_cast<uint32_t>(pipelineState.depthOpFail));
            key += "|stencilPass:" + std::to_string(static_cast<uint32_t>(pipelineState.bothOpFail));
            return key;
        }

        std::string BuildOverrideKey(const MaterialPipelineStateOverrides& overrides)
        {
            std::string key = "overrides";
            AppendOptionalBool(key, "overrideDepthWrite", overrides.depthWrite);
            AppendOptionalBool(key, "overrideColorWrite", overrides.colorWrite);
            AppendOptionalBool(key, "overrideBlending", overrides.blending);
            AppendOptionalBool(key, "overrideDepthTest", overrides.depthTest);
            AppendOptionalBool(key, "overrideHasDepth", overrides.hasDepthAttachment);
            AppendOptionalBool(key, "overrideCulling", overrides.culling);
            AppendOptionalBool(key, "overrideStencilTest", overrides.stencilTest);
            AppendOptionalUInt(key, "overrideStencilWrite", overrides.stencilWriteMask);
            AppendOptionalUInt(key, "overrideSampleCount", overrides.sampleCount);
            AppendOptionalCompare(key, "overrideDepthCompare", overrides.depthCompare);
            AppendOptionalCullFace(key, "overrideCullFace", overrides.cullFace);
            AppendOptionalTextureFormat(key, "overrideDepthFormat", overrides.depthFormat);
            AppendOptionalColorFormats(
                key,
                overrides.HasColorFormatsOverride(),
                overrides.GetColorFormats());
            AppendOptionalColorSpaces(
                key,
                overrides.HasColorSpacesOverride(),
                overrides.GetColorSpaces());
            AppendOptionalRenderTargetBlendStates(
                key,
                overrides.HasRenderTargetBlendStatesOverride(),
                overrides.GetRenderTargetBlendStates());
            return key;
        }

        void AppendShaderLabKeywords(std::string& key, const Material& material)
        {
            const auto keywords = material.GetShaderLabKeywordNames();
            if (keywords.empty())
                return;

            key += "|keywords:";
            for (size_t index = 0u; index < keywords.size(); ++index)
            {
                if (index > 0u)
                    key += ",";
                key += keywords[index];
            }
        }
    }

    MaterialVariantIdentity BuildMaterialVariantIdentity(const Material& material)
    {
        MaterialVariantIdentity identity;
        if (!material.path.empty())
        {
            identity.stableKey = "path:" + material.path;
            AppendShaderLabKeywords(identity.stableKey, material);
            return identity;
        }

        identity.stableKey = "runtime:";
        if (const auto* shader = material.GetShader(); shader != nullptr)
            identity.stableKey += shader->path;
        AppendBool(identity.stableKey, "depthTest", material.HasDepthTest());
        AppendBool(identity.stableKey, "depthWrite", material.HasDepthWriting());
        AppendBool(identity.stableKey, "colorWrite", material.HasColorWriting());
        AppendBool(identity.stableKey, "blend", material.IsBlendable());
        identity.stableKey += "|surface:";
        identity.stableKey += MaterialSurfaceModeName(material.GetSurfaceMode());
        AppendBool(identity.stableKey, "backCull", material.HasBackfaceCulling());
        AppendBool(identity.stableKey, "frontCull", material.HasFrontfaceCulling());
        AppendShaderLabKeywords(identity.stableKey, material);
        return identity;
    }

    MaterialPassVariantKey BuildMaterialPassVariantKey(
        const Material& material,
        std::string_view passName,
        const Data::PipelineState& pipelineState,
        const MaterialPipelineStateOverrides& overrides)
    {
        MaterialPassVariantKey key;
        key.stableKey = "pass:";
        key.stableKey += passName;
        key.stableKey += "|material:";
        key.stableKey += BuildMaterialVariantIdentity(material).stableKey;
        key.stableKey += "|";
        key.stableKey += BuildPipelineStateKey(pipelineState);
        key.stableKey += "|";
        key.stableKey += BuildOverrideKey(overrides);
        return key;
    }
}
