
#pragma once
#include <map>
#include <functional>
#include <string>
#include "CoreDef.h"
#include "Core/Image.h"
using std::map;

#include "RHI/TextureBase.h"

namespace NLS
{
class Image;
typedef std::function<Image(const std::string& filename, int& flags)> TextureLoadFunction;

typedef std::function<Rendering::TextureBase*(const std::string& filename)> APILoadFunction;

class NLS_CORE_API TextureLoader
{
public:
    static Image LoadTexture(const std::string& filename,int& flags);

    static void RegisterTextureLoadFunction(TextureLoadFunction f, const std::string& fileExtension);

    static void RegisterAPILoadFunction(APILoadFunction f);

    static Rendering::TextureBase* LoadAPITexture(const std::string& filename);

protected:
    static std::string GetFileExtension(const std::string& fileExtension);

    static std::map<std::string, TextureLoadFunction> fileHandlers;

    static APILoadFunction apiFunction;
};
} // namespace NLS
