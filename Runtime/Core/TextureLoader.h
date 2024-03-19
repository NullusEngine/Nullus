/*
Part of Newcastle University's Game Engineering source code.

Use as you see fit!

Comments and queries to: richard-gordon.davison AT ncl.ac.uk
https://research.ncl.ac.uk/game/
*/
#pragma once
#include <map>
#include <functional>
#include <string>
#include "CoreDef.h"
using std::map;

#include "RHI/TextureBase.h"

namespace NLS
{

typedef std::function<bool(const std::string& filename, char*& outData, int& width, int& height, int& channels, int& flags)> TextureLoadFunction;

typedef std::function<Rendering::TextureBase*(const std::string& filename)> APITextureLoadFunction;
typedef std::function<Rendering::TextureBase*(const Rendering::CubeMapFileNames& filenames)> APICubeMapLoadFunction;

class NLS_CORE_API TextureLoader
{
public:
    static bool LoadTexture(const std::string& filename, char*& outData, int& width, int& height, int& channels, int& flags);

    static void RegisterTextureLoadFunction(TextureLoadFunction f, const std::string& fileExtension);

    static void RegisterAPILoadFunction(const APITextureLoadFunction& f);
    static void RegisterAPICubeMapLoadFunction(const APICubeMapLoadFunction& f);

    static Rendering::TextureBase* LoadAPITexture(const std::string& filename);
    static Rendering::TextureBase* LoadAPICubeMap(const Rendering::CubeMapFileNames& filename);

protected:
    static std::string GetFileExtension(const std::string& fileExtension);

    static std::map<std::string, TextureLoadFunction> fileHandlers;

    static APITextureLoadFunction apiFunction;
    static APICubeMapLoadFunction apiCubeMapLoadFunction;
};
} // namespace NLS
