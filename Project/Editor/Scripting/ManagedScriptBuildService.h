#pragma once

#include <filesystem>
#include <future>
#include <string>
#include <vector>

namespace NLS::Editor::Scripting
{
struct ManagedScriptBuildRequest
{
    std::filesystem::path projectRoot;
    std::filesystem::path dotnetPath;
    std::filesystem::path outputRoot;
    std::string configuration = "Debug";
    std::filesystem::path scriptApiManifest;
    std::string expectedSchemaHash;
};

struct ManagedScriptBuildResult
{
    bool succeeded = false;
    int exitCode = -1;
    std::filesystem::path projectRoot;
    std::filesystem::path assembly;
    std::filesystem::path symbols;
    std::filesystem::path deps;
    std::filesystem::path runtimeConfig;
    std::filesystem::path sarifDiagnostics;
    std::vector<std::filesystem::path> sarifDiagnosticFiles;
    std::string output;
};

struct ManagedBuildDiagnostic
{
    enum class Severity { Info, Warning, Error };
    Severity severity = Severity::Error;
    std::string message;
    std::string sourcePath;
    int line = 0;
    int column = 0;
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
    static std::vector<ManagedBuildDiagnostic> ReadDiagnostics(const ManagedScriptBuildResult& result);

private:
    std::future<ManagedScriptBuildResult> m_activeBuild;
};
}
