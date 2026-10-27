#pragma once

#include "BuildDef.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Build
{
// Build modes follow the engine's shipped configurations. Development keeps
// debug connections and developer fallbacks; Shipping removes them and demands
// a complete, self-contained payload.
enum class BuildMode
{
    Development,
    Shipping
};

enum class PlatformFamily
{
    Windows,
    Linux,
    MacOS,
    Android,
    IOS,
    Web,
    Unknown
};

enum class Architecture
{
    X64,
    Arm64,
    Unknown
};

enum class GraphicsBackendKind
{
    DX12,
    Vulkan,
    Metal,
    WebGPU,
    OpenGL,
    None
};

enum class OutputFormat
{
    Zip,
    TarZst,
    AppImage,
    Dmg,
    Apk,
    Aab,
    Ipa,
    Directory
};

enum class ScriptBackend
{
    CSharp,
    Lua
};

enum class DiagnosticSeverity
{
    Info,
    Warning,
    Error
};

struct BuildDiagnostic
{
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    std::string code;
    std::string message;
    std::string detail;

    bool IsError() const { return severity == DiagnosticSeverity::Error; }
};

// System, CPU, graphics backend, minimum OS, content format, and script
// execution model for one build target.
struct BuildTarget
{
    PlatformFamily platform = PlatformFamily::Windows;
    Architecture architecture = Architecture::X64;
    GraphicsBackendKind graphicsBackend = GraphicsBackendKind::DX12;
    std::string minimumOSVersion;
    // ArtifactDB platform tag cooked content must carry ("win64", "linux", ...).
    std::string artifactPlatform;
    // Shader bytecode form produced for this target.
    std::string shaderTarget;
    // Texture format policy: target-native conversion is the only shipping
    // policy; "passthrough" exists for validation-only profiles.
    std::string textureFormatPolicy = "target-native";
    // Optional texture resolution cap for this profile (0 keeps source size).
    // Values are clamped to a power of two by the cooker.
    uint32_t maxTextureSize = 0u;
    bool metalLibraryForApple = false;
    std::vector<ScriptBackend> scriptBackends {ScriptBackend::CSharp, ScriptBackend::Lua};
};

// Product information, version, target, scenes, content rules, build mode,
// icon, output format, and signing references. Persisted in the project's
// versioned BuildProfiles.json.
struct BuildProfile
{
    std::string id;
    std::string displayName;
    std::string productName;
    std::string companyName = "Nullus";
    std::string version = "0.1.0";
    std::string buildNumber;
    BuildMode mode = BuildMode::Development;
    BuildTarget target;

    // Project-relative path of the scene the player boots into.
    std::string startScene;
    // Pack selections ("packName" or "packName:variant") built as separate
    // packages in addition to the base package.
    std::vector<std::string> assetPackSelections;
    // Explicit keep rules for dynamically loaded content. Entries are
    // project-relative paths, directory prefixes, or GUIDs.
    std::vector<std::string> keepAssetPaths;
    // Raw asset extensions copied loose for phase-1 runtime readers.
    std::vector<std::string> rawAssetExtensions {
        ".lua", ".ui", ".style", ".loc", ".csv", ".fnt", ".ttf", ".otf", ".json"};

    std::string iconPath;
    std::string executableName;
    OutputFormat outputFormat = OutputFormat::Zip;
    // Optional override for the output root; defaults to <project>/Dist.
    std::string outputDirectory;
    // Prebuilt player template directory; empty selects the editor-adjacent
    // default for the active build configuration.
    std::string playerTemplatePath;
    // Signing credential reference. The credential itself is resolved from
    // the system credential store or CI secrets, never from the project.
    std::string signingProfile;
    bool includeDebugSymbols = true;
    // Native modules that force a source-built player instead of the
    // precompiled template.
    std::vector<std::string> customNativeModules;
    // Launch validation after packaging (used by CI and manual acceptance).
    bool validateLaunchOnBuild = false;
};

struct PlatformCapability
{
    PlatformFamily platform = PlatformFamily::Unknown;
    Architecture architecture = Architecture::Unknown;
    GraphicsBackendKind graphicsBackend = GraphicsBackendKind::None;
    // True when this editor/tool binary contains a working adapter.
    bool adapterAvailable = false;
    bool toolchainAvailable = false;
    bool deviceValidationRequired = true;
    std::string id;
    std::string reason;
};

struct BuildPhaseTiming
{
    std::string phase;
    double seconds = 0.0;
};

// Product information resolved from a profile plus the project settings.
struct BuildProductInfo
{
    std::string productName;
    std::string companyName;
    std::string version;
    std::string buildNumber;
    std::string executableName;
    std::string playerId;
    std::string engineVersion;
};

// String conversions shared by the profile IO and the CLI.
NLS_BUILD_API const char* ToString(BuildMode value);
NLS_BUILD_API const char* ToString(PlatformFamily value);
NLS_BUILD_API const char* ToString(Architecture value);
NLS_BUILD_API const char* ToString(GraphicsBackendKind value);
NLS_BUILD_API const char* ToString(OutputFormat value);
NLS_BUILD_API const char* ToString(ScriptBackend value);
NLS_BUILD_API BuildMode BuildModeFromString(const std::string& value, BuildMode fallback = BuildMode::Development);
NLS_BUILD_API PlatformFamily PlatformFamilyFromString(const std::string& value, PlatformFamily fallback = PlatformFamily::Unknown);
NLS_BUILD_API Architecture ArchitectureFromString(const std::string& value, Architecture fallback = Architecture::Unknown);
NLS_BUILD_API GraphicsBackendKind GraphicsBackendKindFromString(
    const std::string& value,
    GraphicsBackendKind fallback = GraphicsBackendKind::None);
NLS_BUILD_API OutputFormat OutputFormatFromString(const std::string& value, OutputFormat fallback = OutputFormat::Zip);
NLS_BUILD_API ScriptBackend ScriptBackendFromString(const std::string& value, ScriptBackend fallback = ScriptBackend::Lua);

// Default artifact platform tag for a target ("win64", "linux", "macos").
NLS_BUILD_API std::string DefaultArtifactPlatform(PlatformFamily platform);
// Stable target identifier ("windows-x64-dx12") used in reports and packages.
NLS_BUILD_API std::string BuildTargetId(const BuildTarget& target);
NLS_BUILD_API std::string BuildModeConfiguration(BuildMode mode);
} // namespace NLS::Build
