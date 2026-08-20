#pragma once

#include <filesystem>
#include <future>
#include <string>

namespace NLS::Editor::Scripting
{
struct ManagedScriptBuildRequest
{
    std::filesystem::path projectRoot;
    std::filesystem::path dotnetPath;
    std::filesystem::path outputRoot;
    std::string configuration = "Debug";
};

struct ManagedScriptBuildResult
{
    bool succeeded = false;
    int exitCode = -1;
    std::filesystem::path assembly;
    std::filesystem::path symbols;
    std::filesystem::path deps;
    std::filesystem::path runtimeConfig;
    std::filesystem::path sarifDiagnostics;
    std::string output;
};

class ManagedScriptBuildService final
{
public:
    ManagedScriptBuildService() = default;
    ~ManagedScriptBuildService();

    ManagedScriptBuildService(const ManagedScriptBuildService&) = delete;
    ManagedScriptBuildService& operator=(const ManagedScriptBuildService&) = delete;

    std::future<ManagedScriptBuildResult> Start(ManagedScriptBuildRequest request);
    static ManagedScriptBuildResult Build(const ManagedScriptBuildRequest& request);

    static std::filesystem::path ResolveRepositoryDotnet(const std::filesystem::path& projectRoot);
    static std::string BuildCommand(const ManagedScriptBuildRequest& request);

private:
    std::future<ManagedScriptBuildResult> m_activeBuild;
};
}
