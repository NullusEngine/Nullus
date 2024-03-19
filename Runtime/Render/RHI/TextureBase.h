#pragma once
#include "RenderDef.h"

#include <string>
#include <array>

namespace NLS
{
namespace Rendering
{
using CubeMapFileNames = std::array<std::string, 6>;

class NLS_RENDER_API TextureBase
{
public:
    virtual ~TextureBase();

protected:
    TextureBase();
};
} // namespace Rendering
} // namespace NLS
