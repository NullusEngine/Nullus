#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/ECullFace.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "Rendering/ShaderLab/ShaderLabDiagnostic.h"
#include "Rendering/ShaderLab/ShaderLabSourceLocation.h"

namespace NLS::Render::ShaderLab
{
    using ShaderLabFloat4 = std::array<float, 4>;

    enum class NLS_RENDER_API ShaderLabPropertyType : uint8_t
    {
        Float,
        Int,
        Range,
        Vector,
        Color,
        Texture2D,
        TextureCube
    };

    enum class NLS_RENDER_API ShaderLabCullMode : uint8_t
    {
        Off,
        Front,
        Back
    };

    enum class NLS_RENDER_API ShaderLabShaderModel : uint8_t
    {
        SM6_0,
        SM6_6
    };

    using ShaderLabValueVariant = std::variant<std::monostate, float, int32_t, ShaderLabFloat4, std::string>;

    struct NLS_RENDER_API ShaderLabPropertyDesc
    {
        std::string name;
        std::string displayName;
        ShaderLabPropertyType type = ShaderLabPropertyType::Float;
        ShaderLabValueVariant defaultValue = 0.0f;
        float rangeMin = 0.0f;
        float rangeMax = 1.0f;
        ShaderLabSourceLocation location;
    };

    struct NLS_RENDER_API ShaderLabTagSet
    {
        std::unordered_map<std::string, std::string> values;
    };

    struct NLS_RENDER_API ShaderLabKeywordPragma
    {
        std::vector<std::string> keywords;
        ShaderLabSourceLocation location;
    };

    struct NLS_RENDER_API ShaderLabPassState
    {
        ShaderLabCullMode cullMode = ShaderLabCullMode::Back;
        bool depthWrite = true;
        NLS::Render::Settings::EComparaisonAlgorithm depthCompare =
            NLS::Render::Settings::EComparaisonAlgorithm::LESS;
        NLS::Render::RHI::RHIBlendStateDesc blend{};
    };

    struct NLS_RENDER_API ShaderLabPassDesc
    {
        std::string name;
        uint32_t subShaderIndex = 0;
        uint32_t passIndex = 0;
        ShaderLabTagSet tags;
        ShaderLabPassState state;
        std::string hlslSource;
        ShaderLabSourceLocation hlslLocation;
        std::string vertexEntry;
        std::string fragmentEntry;
        std::string computeEntry;
        std::vector<ShaderLabKeywordPragma> shaderFeatures;
        std::vector<ShaderLabKeywordPragma> multiCompiles;
    };

    struct NLS_RENDER_API ShaderLabSubShaderDesc
    {
        ShaderLabTagSet tags;
        std::vector<ShaderLabPassDesc> passes;
    };

    struct NLS_RENDER_API ShaderLabAssetDesc
    {
        std::string shaderName;
        std::vector<ShaderLabPropertyDesc> properties;
        std::vector<ShaderLabSubShaderDesc> subShaders;
        std::string fallbackShader;
    };

    NLS_RENDER_API const char* ToString(ShaderLabPropertyType type);
    NLS_RENDER_API const char* ToString(ShaderLabCullMode mode);
    NLS_RENDER_API NLS::Render::Settings::ECullFace ToRhiCullFace(ShaderLabCullMode mode);
    NLS_RENDER_API NLS::Render::RHI::RHIRasterStateDesc ToRhiRasterState(ShaderLabCullMode mode);
}
