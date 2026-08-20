#pragma once

#include <filesystem>
#include <optional>

namespace NLS::Editor::Scripting
{
struct ManagedScriptProjectPaths
{
    std::filesystem::path projectRoot;
    std::filesystem::path managedProjectRoot;
    std::filesystem::path projectFile;
};

inline std::optional<ManagedScriptProjectPaths> ResolveManagedScriptProject(
    const std::filesystem::path& projectRoot)
{
    if (projectRoot.empty())
        return std::nullopt;

    std::error_code error;
    auto normalizedRoot = std::filesystem::weakly_canonical(projectRoot, error);
    if (error)
        normalizedRoot = projectRoot.lexically_normal();
    if (normalizedRoot.empty())
        return std::nullopt;

    for (auto candidate = normalizedRoot; !candidate.empty(); candidate = candidate.parent_path())
    {
        const auto projectFile =
            candidate / "Managed" / "Nullus.GameScripts" / "Nullus.GameScripts.csproj";
        if (std::filesystem::is_regular_file(projectFile, error))
        {
            return ManagedScriptProjectPaths {
                normalizedRoot,
                candidate,
                projectFile
            };
        }
        if (candidate == candidate.parent_path())
            break;
    }
    return std::nullopt;
}
}
