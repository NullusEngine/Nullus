/*
Part of Newcastle University's Game Engineering source code.

Use as you see fit!

Comments and queries to: richard-gordon.davison AT ncl.ac.uk
https://research.ncl.ac.uk/game/
*/
#include "TextureWriter.h"
#include "Assets.h"
using namespace NLS;

void TextureWriter::WritePNG(const std::string& filename, const Image& image)
{
    image.Save(filename);
}
