#pragma once

#include "BuildDef.h"
#include "BuildTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Build
{
struct BuildIncludedAsset
{
    std::string reference;   // GUID or sub-asset reference text
    std::string sourcePath;  // project-relative source path when known
    std::string artifactPath;
    std::string package;     // package that carries the artifact
    std::string reason;      // why the asset is included
    uint64_t size = 0u;
    bool deduplicated = false;
};

struct BuildOutputFile
{
    std::string path;      // output-relative path
    std::string kind;      // "executable", "package", "manifest", "archive", ...
    uint64_t size = 0u;
    std::string sha256;
};

// Everything a successful (or failed) build records. The report is written
// next to the product as BuildReport.json and is the input of `validate`.
struct NLS_BUILD_API BuildReport
{
    std::string schemaVersion = "1";
    bool succeeded = false;
    std::string profileId;
    std::string profileName;
    std::string buildMode;
    std::string targetId;
    std::string artifactPlatform;
    std::string productName;
    std::string productVersion;
    std::string buildNumber;
    std::string engineVersion;
    std::string playerId;
    std::string playerTemplatePath;
    std::string configuration;
    std::string inputHash;
    std::string startedUtc;
    std::string finishedUtc;
    std::string outputDirectory;
    std::string packageDirectory;
    std::vector<BuildPhaseTiming> phases;
    std::vector<BuildDiagnostic> diagnostics;
    std::vector<BuildIncludedAsset> includedAssets;
    std::vector<BuildOutputFile> outputs;
    std::vector<std::string> contentPackages;
    std::vector<std::string> cachedStages;
    bool signed_ = false;
    std::string signingDescription;

    bool HasErrors() const;
    size_t ErrorCount() const;
    size_t WarningCount() const;
    void AddDiagnostic(DiagnosticSeverity severity, std::string code, std::string message, std::string detail = {});

    std::string ToJson() const;
    static std::optional<BuildReport> FromJson(const std::string& json, std::string* error);

    bool Save(const std::filesystem::path& path, std::string* error) const;
    static std::optional<BuildReport> Load(const std::filesystem::path& path, std::string* error);

    // Formats a human-readable summary used by the CLI and CI logs.
    std::string FormatSummary() const;
};

NLS_BUILD_API std::string ComputeFileSha256(const std::filesystem::path& path);
NLS_BUILD_API std::string ComputeStringSha256(const std::string& text);
NLS_BUILD_API std::string CurrentUtcTimestamp();
} // namespace NLS::Build
