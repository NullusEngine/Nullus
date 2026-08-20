#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Debug
{
struct EngineWorkspaceProject
{
    std::string name;
    std::string path;
    std::string sourceRoot;
    std::string kind = "native";
};

struct EngineWorkspaceDescriptor
{
    uint32_t schemaVersion = 1u;
    bool visualStudioSupported = false;
    std::string generator;
    std::filesystem::path sourceRoot;
    std::filesystem::path buildRoot;
    std::filesystem::path cmakeExecutable;
    std::filesystem::path debugPdb;
    std::filesystem::path releasePdb;
    std::string editorTarget = "Editor";
    std::vector<EngineWorkspaceProject> projects;
};

struct ProjectDebugManifest
{
    uint32_t schemaVersion = 3u;
    uint32_t protocolMajor = 2u;
    uint32_t protocolMinor = 0u;
    uint64_t workspaceRevision = 0u;
    std::string projectId;
    std::string engineBuildId;
    std::string brokerVersion;
    std::string requiredVsixVersion;
    std::filesystem::path projectRoot;
    std::filesystem::path projectFile;
    std::filesystem::path workspaceRoot;
    std::filesystem::path visualStudioSolution;
    std::filesystem::path visualStudioScriptsFilter;
    std::filesystem::path visualStudioEngineFilter;
    std::filesystem::path visualStudioProject;
    std::filesystem::path visualStudioCodeWorkspace;
    std::filesystem::path visualStudioExtension;
    std::filesystem::path editorExecutable;
    std::filesystem::path brokerExecutable;
    std::filesystem::path engineWorkspaceDescriptor;
    std::filesystem::path engineSourceRoot;
    std::filesystem::path engineBuildRoot;
    std::filesystem::path engineSolution;
    std::filesystem::path cmakeExecutable;
    std::string nativeTarget = "Editor";
    std::string nativeConfiguration = "Debug";
    std::string nativeBuildId;
    std::string nativePdbSignature;
    bool engineSourceAvailable = false;
    bool nativeSymbolsAvailable = false;
    bool mixedDebugAvailable = false;
    std::vector<EngineWorkspaceProject> engineProjects;
};

struct ProjectDebugWorkspaceResult
{
    bool success = false;
    ProjectDebugManifest manifest;
    std::string errorMessage;
};

// Returns the canonical project root for either a project directory or a
// .nullus file. The helper is intentionally independent from the Launcher so
// IDE integrations can resolve a project without starting an Editor.
std::filesystem::path ResolveProjectRoot(const std::filesystem::path& projectPath);

// Project identity is stable across launches and isolated between projects.
// Windows paths are folded to lower case; Unix paths retain case.
std::string ComputeProjectId(const std::filesystem::path& projectPath);

ProjectDebugWorkspaceResult GenerateProjectDebugWorkspace(
    const std::filesystem::path& projectPath,
    const std::filesystem::path& editorExecutable = {},
    const std::filesystem::path& brokerExecutable = {},
    uint16_t luaPandaPort = 8818u,
    bool stopOnEntry = false);

std::optional<ProjectDebugManifest> ReadProjectDebugManifest(
    const std::filesystem::path& projectRoot,
    std::string* errorMessage = nullptr);

std::optional<EngineWorkspaceDescriptor> ReadEngineWorkspaceDescriptor(
    const std::filesystem::path& descriptorPath,
    std::string* errorMessage = nullptr);
}
