#pragma once
#include <span>
#include "ShaderReflection.h"

namespace NLS::Render::Resources
{
// Converts the existing DXC register shifts back to logical HLSL coordinates.
NLS_RENDER_API bool ReflectSpirvShader(std::span<const uint8_t> bytecode,
    ShaderCompiler::ShaderStage stage, ShaderReflection& reflection, std::string& error);
}
