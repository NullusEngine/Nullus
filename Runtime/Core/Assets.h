#pragma once
#include <string>
#include "CoreDef.h"
namespace NLS
{
namespace Assets
{
const std::string SHADERDIR("../../App/Assets/Shaders/");
const std::string MESHDIR("../../App/Assets/Meshes/");
const std::string TEXTUREDIR("../../App/Assets/Textures/");
const std::string SOUNDSDIR("../../App/Assets/Sounds/");
const std::string FONTSSDIR("../../App/Assets/Fonts/");
const std::string DATADIR("../../App/Assets/Data/");
const std::string LOGDIR("../../App/Log/");
extern NLS_CORE_API bool ReadTextFile(const std::string& filepath, std::string& result);
extern NLS_CORE_API bool ReadBinaryFile(const std::string& filepath, char** into, size_t& size);
} // namespace Assets
} // namespace NLS