#pragma once

#include "CookDef.h"

#include "Assets/AssetMeta.h"
#include "Guid.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Cook
{
enum class CookSeverity
{
    Info,
    Warning,
    Error
};

struct CookDiagnostic
{
    CookSeverity severity = CookSeverity::Info;
    std::string code;
    std::string message;
    std::string path;
};

// The target capabilities a cook run produces content for. Desktop tooling
// maps this from BuildTarget; the cook library deliberately does not depend on
// the build library so the editor can reuse it without the packaging layer.
struct CookTarget
{
    std::string platform = "windows";
    std::string architecture = "x64";
    std::string graphicsBackend = "dx12";
    std::string artifactPlatform = "win64";
    std::string shaderTarget = "dxil";
    std::string textureFormatPolicy = "target-native";
    uint32_t maxTextureSize = 0u;
};

struct CookOptions
{
    std::filesystem::path projectRoot;
    std::filesystem::path assetsRoot;
    std::filesystem::path artifactDatabasePath;
    std::filesystem::path artifactRoot;
    std::filesystem::path stagingRoot;
    CookTarget target;
    bool force = false;
    bool cookUnreferenced = true;
    // Optional override for the profile texture resolution cap.
    uint32_t maxTextureSize = 0u;
    // Optional case-insensitive substring filter for targeted bake runs.
    std::string pathFilter;
    std::atomic_bool* cancel = nullptr;
    std::function<void(const std::string& assetPath, size_t completed, size_t total)> progress;
};

struct CookAssetOutcome
{
    std::string assetPath;
    std::string artifactPath;
    std::string subAssetKey;
    bool succeeded = false;
    bool skipped = false;
    bool unsupported = false;
    std::string error;
};

struct NLS_COOK_API CookSummary
{
    size_t total = 0u;
    size_t cooked = 0u;
    size_t skipped = 0u;
    size_t unsupported = 0u;
    size_t failed = 0u;
    std::vector<CookDiagnostic> diagnostics;
    std::vector<CookAssetOutcome> assets;

    bool Succeeded() const { return failed == 0u; }
    size_t ErrorCount() const;
    size_t WarningCount() const;
    void AddDiagnostic(CookSeverity severity, std::string code, std::string message, std::string path = {});
    std::string FormatSummary() const;
};

// Shared, headless asset cooking used by the editor and NullusBuild.
//
// Covered asset types:
//   - Texture          (target-native BC or RGBA8 artifact with mip chain)
//   - Shader           (ShaderLab passes compiled to target bytecode)
//   - Prefab           (validated raw ObjectGraph document)
//   - UIDocument       (native UI document artifact)
//   - UIStyleSheet     (native style artifact)
//   - LocalizationTable(native localization artifact)
//
// Model scenes, materials and audio are reported as unsupported with a
// precise diagnostic until their editor importers are moved into this
// library. The packaging pipeline treats an unsupported asset as a warning
// unless the dependency closure references it, in which case the build fails
// with the missing artifact.
class NLS_COOK_API AssetCooker
{
public:
    static CookSummary CookProject(const CookOptions& options);
    static CookSummary CookAssets(
        const CookOptions& options,
        const std::vector<std::filesystem::path>& assetPaths);
    static bool IsSupportedAssetType(NLS::Core::Assets::AssetType type);

    // Cached-result probe used by the packaging pipeline: returns the cooked
    // primary artifact path for an asset when the ArtifactDB already holds an
    // up-to-date record for the requested target.
    static std::optional<std::string> FindUpToDatePrimaryArtifact(
        const std::filesystem::path& artifactDatabasePath,
        NLS::Core::Assets::AssetId assetId,
        const std::string& targetPlatform);
};

// Engine-bundled assets live beside the executable instead of inside a
// project. They are addressed with the same virtual paths materials use
// (Assets/Engine/...), and receive stable identities so a cold build can
// publish and reference them without an editor-side .meta file.
NLS_COOK_API NLS::Guid MakeEngineAssetGuid(std::string_view virtualPath);
NLS_COOK_API bool IsEngineVirtualAssetPath(std::string_view virtualPath);
} // namespace NLS::Cook
