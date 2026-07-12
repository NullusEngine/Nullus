#pragma once

#include <string>

namespace NLS::Editor::Assets
{
inline constexpr const char* kDefaultShaderLabMaterialShaderPath =
    "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader";

inline std::string BuildDefaultShaderLabMaterialPayload()
{
    std::string payload;
    payload += "shaderLabMaterialVersion=1\n";
    payload += "shader=";
    payload += kDefaultShaderLabMaterialShaderPath;
    payload += "\n";
    payload += "surfaceMode=Opaque\n";
    payload += "alphaMode=Opaque\n";
    payload += "doubleSided=true\n";
    payload += "depthWrite=true\n";
    return payload;
}
}
