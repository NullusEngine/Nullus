#include "Assets/EditorAssetPathUtils.h"

#include <filesystem>

namespace NLS::Editor::Assets
{
bool IsBuiltInResourcePath(const std::string& resourcePath)
{
    return (!resourcePath.empty() && resourcePath.front() == ':') ||
        resourcePath.rfind("builtin:", 0) == 0;
}

std::string GetBuiltInResourceDisplayName(const std::string& resourcePath)
{
    std::string normalized = resourcePath;
    if (IsBuiltInResourcePath(normalized))
        normalized.erase(normalized.begin());

    for (auto& ch : normalized)
    {
        if (ch == '\\')
            ch = '/';
    }

    auto name = std::filesystem::path(normalized).stem().generic_string();
    if (name.empty())
        name = std::filesystem::path(normalized).filename().generic_string();
    return name.empty() ? resourcePath : name;
}
}
