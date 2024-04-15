/*
Part of Newcastle University's Game Engineering source code.

Use as you see fit!

Comments and queries to: richard-gordon.davison AT ncl.ac.uk
https://research.ncl.ac.uk/game/
*/
#include "TextureLoader.h"
#include <iostream>
#include "Assets.h"

#ifdef WIN32
    #include <filesystem>
using namespace std::filesystem;
#endif

using namespace NLS;
using namespace Rendering;

std::map<std::string, TextureLoadFunction> TextureLoader::fileHandlers;
APILoadFunction TextureLoader::apiFunction = nullptr;

Image TextureLoader::LoadTexture(const std::string& filename, int& flags)
{

    std::string extension = GetFileExtension(filename);

    auto it = fileHandlers.find(extension);

    std::string realPath = Assets::TEXTUREDIR + filename;

    if (it != fileHandlers.end())
    {
        // There's a custom handler function for this, just use that
        return it->second(realPath, flags);
    }

    return Image(realPath);
}

void TextureLoader::RegisterTextureLoadFunction(TextureLoadFunction f, const std::string& fileExtension)
{
    fileHandlers.insert(std::make_pair(fileExtension, f));
}

std::string TextureLoader::GetFileExtension(const std::string& fileExtension)
{
#ifdef WIN32
    path p = path(fileExtension);

    path ext = p.extension();

    return ext.string();
#else
    return std::string();
#endif
}

void TextureLoader::RegisterAPILoadFunction(APILoadFunction f)
{
    if (apiFunction)
    {
        std::cout << __FUNCTION__ << " replacing previously defined API function." << std::endl;
    }
    apiFunction = f;
}

TextureBase* TextureLoader::LoadAPITexture(const std::string& filename)
{
    if (apiFunction == nullptr)
    {
        std::cout << __FUNCTION__ << " no API Function has been defined!" << std::endl;
        return nullptr;
    }
    return apiFunction(filename);
}