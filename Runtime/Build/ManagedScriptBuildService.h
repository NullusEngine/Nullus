#pragma once

#include "BuildDef.h"

#include <filesystem>
#include <string>

namespace NLS::Build
{
struct ManagedScriptBuildRequest
{
    std::filesystem::path projectRoot;
    std::filesystem::path dotnetPath;
    std::filesystem::path outputRoot;
    std::string configuration = "Release";
};

struct ManagedScriptBuildResult
{
    bool succeeded = false;
    int exitCode = -1;
    std::filesystem::path assembly;
    std::filesystem::path deps;
    std::filesystem::path runtimeConfig;
    std::filesystem::path engineAssembly;
    std::filesystem::path engineDeps;
    std::filesystem::path engineRuntimeConfig;
    std::filesystem::path managedAssembly;
    std::string output;
};

// Builds a project's own Assets/**/*.cs into GameScripts.dll with the
// repository's managed SDK. Packaging uses this instead of the sample
// GameScripts assembly that may be pre-copied into a player template.
class NLS_BUILD_API ManagedScriptBuildService
{
public:
    static ManagedScriptBuildResult Build(const ManagedScriptBuildRequest& request);
    static std::filesystem::path ResolveRepositoryDotnet(
        const std::filesystem::path& projectRoot);
    static std::string BuildCommand(const ManagedScriptBuildRequest& request);
};
} // namespace NLS::Build
