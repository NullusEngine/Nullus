/*
Part of Newcastle University's Game Engineering source code.

Use as you see fit!

Comments and queries to: richard-gordon.davison AT ncl.ac.uk
https://research.ncl.ac.uk/game/
*/
#pragma once
#include <string>
#include "CoreDef.h"
#include "Image.h"
namespace NLS
{
class NLS_CORE_API TextureWriter
{
public:
    static void WritePNG(const std::string& filename, const Image& image);
};
} // namespace NLS
