#pragma once

#include <filesystem>
#include <optional>
#include <vector>
#include <utility>
#include <Platform/Process/Process.h>

namespace NLS::Editor::Scripting
{
struct ManagedScriptProjectPaths
{
    std::filesystem::path projectRoot;
    std::filesystem::path managedProjectRoot;
    std::filesystem::path projectFile;
};

inline std::optional<ManagedScriptProjectPaths> ResolveManagedScriptProject(
    const std::filesystem::path& projectRoot,
    std::filesystem::path sdkRoot = NLS::Platform::Process::ResolveInstallResourceRoots().installRoot)
{
    if (projectRoot.empty())
        return std::nullopt;

    std::error_code error;
    auto normalizedRoot = std::filesystem::weakly_canonical(projectRoot, error);
    if (error)
        normalizedRoot = projectRoot.lexically_normal();
    if (normalizedRoot.empty())
        return std::nullopt;

    // A project can carry its own SDK. Otherwise build with the SDK belonging
    // to this Editor before considering an unrelated containing checkout.
    std::vector<std::filesystem::path> probes {normalizedRoot};
    const auto appendAncestors = [&probes](std::filesystem::path candidate)
    {
        for (; !candidate.empty(); candidate = candidate.parent_path())
        {
            probes.push_back(candidate);
            if (candidate == candidate.parent_path()) break;
        }
    };
    appendAncestors(std::move(sdkRoot));
    appendAncestors(normalizedRoot.parent_path());
    for (const auto& candidate : probes)
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
    }
    return std::nullopt;
}
}
