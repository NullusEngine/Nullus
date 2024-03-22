#pragma once
#include "RenderDef.h"

#include <string>
#include <array>

namespace NLS
{
namespace Rendering
{
enum class TextureType
{
    e2D,
    eCube,
};

using CubeMapFileNames = std::array<std::string, 6>;

class NLS_RENDER_API TextureBase
{
public:
    virtual ~TextureBase();

protected:
    TextureBase();

	TextureType textureType = TextureType::e2D;
};
} // namespace Rendering
} // namespace NLS
