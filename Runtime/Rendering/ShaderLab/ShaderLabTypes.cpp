#include "Rendering/ShaderLab/ShaderLabTypes.h"

namespace NLS::Render::ShaderLab
{
    const char* ToString(const ShaderLabPropertyType type)
    {
        switch (type)
        {
        case ShaderLabPropertyType::Float: return "Float";
        case ShaderLabPropertyType::Int: return "Int";
        case ShaderLabPropertyType::Range: return "Range";
        case ShaderLabPropertyType::Vector: return "Vector";
        case ShaderLabPropertyType::Color: return "Color";
        case ShaderLabPropertyType::Texture2D: return "Texture2D";
        case ShaderLabPropertyType::TextureCube: return "TextureCube";
        default: return "Unknown";
        }
    }

    const char* ToString(const ShaderLabCullMode mode)
    {
        switch (mode)
        {
        case ShaderLabCullMode::Off: return "Off";
        case ShaderLabCullMode::Front: return "Front";
        case ShaderLabCullMode::Back: return "Back";
        default: return "Unknown";
        }
    }

    NLS::Render::Settings::ECullFace ToRhiCullFace(const ShaderLabCullMode mode)
    {
        switch (mode)
        {
        case ShaderLabCullMode::Front: return NLS::Render::Settings::ECullFace::FRONT;
        case ShaderLabCullMode::Back: return NLS::Render::Settings::ECullFace::BACK;
        case ShaderLabCullMode::Off:
        default:
            return NLS::Render::Settings::ECullFace::BACK;
        }
    }

    NLS::Render::RHI::RHIRasterStateDesc ToRhiRasterState(const ShaderLabCullMode mode)
    {
        NLS::Render::RHI::RHIRasterStateDesc raster;
        raster.cullEnabled = mode != ShaderLabCullMode::Off;
        raster.cullFace = ToRhiCullFace(mode);
        return raster;
    }
}
