#include "Assets/AssetPath.h"

#include <algorithm>
#include <cctype>

namespace
{
std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}
}

namespace NLS::Core::Assets
{
std::filesystem::path NormalizeAssetPath(const std::filesystem::path& path)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error)
        return path.lexically_normal();
    return absolute.lexically_normal();
}

bool IsMetaFilePath(const std::filesystem::path& path)
{
    return ToLower(path.extension().string()) == ".meta";
}
}
