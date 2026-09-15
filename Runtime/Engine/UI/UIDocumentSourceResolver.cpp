#include "UIDocumentSourceResolver.h"

#include <filesystem>
#include <utility>

namespace NLS::Engine::UI
{
namespace
{
UIDocumentSourcePathResolver& ResolverStorage()
{
    static UIDocumentSourcePathResolver resolver;
    return resolver;
}

bool IsExistingFile(const std::string& path)
{
    if (path.empty())
        return false;
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

// Hosts hand over their assets folder either plainly (".../Assets") or with a
// trailing separator (the editor's projectAssetsPath and the game context both
// keep one). A trailing separator makes parent_path() stop at the assets folder
// itself, so "Assets/..." references would nest a second Assets directory.
std::filesystem::path NormalizeAssetRootPath(const std::filesystem::path& path)
{
    if (path.empty())
        return {};

    auto text = path.generic_string();
    while (text.size() > 1u && text.back() == '/')
    {
        if (text.size() == 3u && text[1] == ':')
            break; // Keep a drive root such as "D:/" intact.
        text.pop_back();
    }
    return std::filesystem::path(text);
}
}

void SetUIDocumentSourcePathResolver(UIDocumentSourcePathResolver resolver)
{
    ResolverStorage() = std::move(resolver);
}

std::string ResolveUIDocumentSourcePath(const UIDocumentReference& reference)
{
    const auto& storedPath = reference.GetResourcePath();
    if (storedPath.empty())
        return {};

    // An already-resolved path (absolute or a project that stores full paths)
    // keeps working without a host resolver.
    if (IsExistingFile(storedPath))
        return storedPath;

    const auto& resolver = ResolverStorage();
    return resolver ? resolver(reference) : std::string {};
}

std::string ResolveProjectUIDocumentPath(
    const std::string& projectAssetsPath,
    const std::string& resourcePath)
{
    if (projectAssetsPath.empty() || resourcePath.empty())
        return {};

    const auto assetsPath = NormalizeAssetRootPath(std::filesystem::path(projectAssetsPath));
    if (assetsPath.empty())
        return {};
    const std::filesystem::path source(resourcePath);
    const auto normalizedSource = source.lexically_normal();
    if (normalizedSource.is_absolute())
        return {};

    const auto genericSource = normalizedSource.generic_string();
    const auto resolved = genericSource == "Assets" || genericSource.rfind("Assets/", 0u) == 0u
        ? (assetsPath.parent_path() / normalizedSource).lexically_normal()
        : (assetsPath / normalizedSource).lexically_normal();

    // A scene may only reference documents below Assets. Absolute paths are for
    // tools and explicit development workflows, never for "../" escapes.
    const auto relativeToAssets = resolved.lexically_relative(assetsPath.lexically_normal());
    if (relativeToAssets.empty() || relativeToAssets.is_absolute())
        return {};
    for (const auto& part : relativeToAssets)
    {
        if (part == "..")
            return {};
    }

    return IsExistingFile(resolved.generic_string()) ? resolved.generic_string() : std::string {};
}
}
