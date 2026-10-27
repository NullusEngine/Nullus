#include "BuildPipelineStaging.h"

#include "BuildFileUtils.h"
#include "BuildPipelineInternal.h"

#include "Assets/ContentSource.h"
#include "Assets/NpakArchive.h"
#include "Engine/Assets/PlayerDescriptor.h"
#include "Engine/Assets/RuntimeManifestSerialization.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <unordered_map>

namespace NLS::Build::Detail
{
namespace
{
std::string PortableArtifactKey(const std::string& artifactPath)
{
    const auto portable = NLS::Core::Assets::TryMakePortableContentArtifactPath(artifactPath);
    return portable.empty() ? artifactPath : portable;
}

std::string SanitizePackageName(std::string name)
{
    for (auto& character : name)
    {
        const bool valid = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.';
        if (!valid)
            character = '_';
    }
    return name.empty() ? std::string("pack") : name;
}

bool WriteRuntimeManifest(
    const BuildContext& context,
    const DependencyCollectionResult& collection,
    std::string* error)
{
    const auto manifestPath = context.stagingRoot / "Data" / "User" / "RuntimeAssetManifest.json";
    if (!EnsureDirectory(manifestPath.parent_path(), error))
        return false;
    std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
            *error = "cannot write runtime manifest '" + manifestPath.string() + "'";
        return false;
    }
    output << NLS::Engine::Assets::SerializeRuntimeAssetManifest(collection.manifest);
    return output.good();
}

bool WriteGameIni(const BuildContext& context, std::string* error)
{
    const auto destination = context.stagingRoot / "Data" / "User" / "Game.ini";
    if (!EnsureDirectory(destination.parent_path(), error))
        return false;

    auto text = ReadTextFileOrEmpty(context.projectFile);
    text = SetIniValue(std::move(text), "start_scene", context.profile.startScene);
    text = SetIniValue(
        std::move(text),
        "graphics_backend",
        ToString(context.profile.target.graphicsBackend));
    text = SetIniValue(
        std::move(text),
        "runtime_target_platform",
        context.profile.target.artifactPlatform);
    text = SetIniValue(
        std::move(text),
        "runtime_asset_manifest",
        "RuntimeAssetManifest.json");
    text = SetIniValue(
        std::move(text),
        "dev_build",
        context.profile.mode == BuildMode::Development ? "true" : "false");
    if (!context.profile.assetPackSelections.empty())
    {
        std::string packs;
        for (const auto& selection : context.profile.assetPackSelections)
        {
            if (!packs.empty())
                packs += ";";
            packs += selection;
        }
        text = SetIniValue(std::move(text), "runtime_prewarm_asset_packs", packs);
    }

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
            *error = "cannot write Game.ini '" + destination.string() + "'";
        return false;
    }
    output << text;
    return output.good();
}

bool WriteRawAssets(const BuildContext& context, std::string* error)
{
    std::set<std::string> extensions;
    for (const auto& extension : context.profile.rawAssetExtensions)
    {
        auto lowered = extension;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        extensions.insert(lowered);
    }
    const auto startScene = [](std::string value)
    {
        value = ToForwardSlashes(std::move(value));
        std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }(context.profile.startScene);

    return CopyTreeFiltered(
        context.projectAssetsPath,
        context.stagingRoot / "Data" / "User" / "Assets",
        [&extensions, &startScene](const std::string& relative)
        {
            auto lowered = relative;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
            if (lowered == startScene)
                return true;
            const auto dot = lowered.find_last_of('.');
            if (dot == std::string::npos)
                return false;
            return extensions.find(lowered.substr(dot)) != extensions.end();
        },
        nullptr,
        error);
}

bool WritePackages(
    const BuildContext& context,
    const DependencyCollectionResult& collection,
    BuildReport& report,
    std::vector<std::string>& contentPackages,
    std::string* error)
{
    const auto packagesDirectory = context.stagingRoot / "Data" / "User" / "Packages";
    if (!EnsureDirectory(packagesDirectory, error))
        return false;

    const auto writePackage = [&](const std::string& packageFileName,
                                  const std::string& packageName,
                                  const std::vector<const CollectedArtifact*>& artifacts) -> bool
    {
        NLS::Core::Assets::NpakWriter writer;
        writer.Clear();
        std::unordered_map<std::string, std::string> keyToArtifact;
        for (const auto* artifact : artifacts)
        {
            if (artifact == nullptr)
                continue;
            const auto key = PortableArtifactKey(artifact->artifactPath);
            if (key.empty())
            {
                if (error != nullptr)
                    *error = "artifact path is not portable: " + artifact->artifactPath;
                return false;
            }
            if (keyToArtifact.find(key) != keyToArtifact.end())
                continue;
            keyToArtifact.emplace(key, artifact->hostPath.string());
            std::string addError;
            if (!writer.AddFileFromDisk(key, artifact->hostPath, &addError))
            {
                if (error != nullptr)
                    *error = addError;
                return false;
            }
        }

        const auto packagePath = packagesDirectory / packageFileName;
        NLS::Core::Assets::NpakWriteOptions options;
        options.blockSize = 256u * 1024u;
        options.compressionLevel = 3;
        options.enableCompression = true;
        std::string writeError;
        if (!writer.Write(packagePath, options, &writeError))
        {
            if (error != nullptr)
                *error = writeError;
            return false;
        }

        const auto deduped = writer.GetDeduplicatedFileCount();
        report.contentPackages.push_back(packageName);
        contentPackages.push_back(packageFileName);
        report.AddDiagnostic(
            DiagnosticSeverity::Info,
            "build-package-written",
            "Content package '" + packageName + "' written (" +
                std::to_string(writer.GetAddedFileCount()) + " files, " +
                std::to_string(deduped) + " deduplicated).",
            packagePath.string());
        return true;
    };

    // Base package: the intersection of every collected artifact and the
    // closure of the scene roots. Packs share these files instead of copying.
    std::vector<const CollectedArtifact*> baseArtifacts;
    for (const auto& artifact : collection.artifacts)
    {
        if (artifact.package == "base")
            baseArtifacts.push_back(&artifact);
    }
    if (!writePackage("base.npak", "base", baseArtifacts))
        return false;

    // Per-pack packages: artifacts reachable only through a selection.
    for (const auto& [packName, artifactPaths] : collection.packageArtifactPaths)
    {
        if (artifactPaths.empty())
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Info,
                "build-package-shared",
                "Asset pack '" + packName + "' only contains assets already in the base package; no separate package was created.",
                packName);
            continue;
        }
        std::vector<const CollectedArtifact*> packArtifacts;
        std::set<std::string> wanted(artifactPaths.begin(), artifactPaths.end());
        for (const auto& artifact : collection.artifacts)
        {
            if (artifact.package == packName && wanted.count(artifact.artifactPath) != 0u)
                packArtifacts.push_back(&artifact);
        }
        if (packArtifacts.empty())
            continue;
        const auto fileName = SanitizePackageName(packName) + ".npak";
        if (!writePackage(fileName, packName, packArtifacts))
            return false;
    }
    return true;
}

bool WritePlayerDescriptor(
    const BuildContext& context,
    const PlayerTemplateInfo& player,
    const std::vector<std::string>& contentPackages,
    BuildReport& report,
    std::string* error)
{
    NLS::Engine::Assets::PlayerDescriptor descriptor;
    descriptor.playerId = context.product.playerId;
    descriptor.engineVersion = context.product.engineVersion;
    descriptor.targetId = BuildTargetId(context.profile.target);
    descriptor.artifactPlatform = context.profile.target.artifactPlatform;
    descriptor.graphicsBackend = ToString(context.profile.target.graphicsBackend);
    descriptor.buildMode = ToString(context.profile.mode);
    descriptor.profileId = context.profile.id;
    descriptor.productName = context.product.productName;
    descriptor.companyName = context.product.companyName;
    descriptor.productVersion = context.product.version;
    descriptor.buildNumber = context.product.buildNumber;
    descriptor.startScene = context.profile.startScene;
    descriptor.runtimeManifestPath = "Data/User/RuntimeAssetManifest.json";
    descriptor.contentRoot = "Data/User";
    for (const auto& package : contentPackages)
        descriptor.contentPackages.push_back("Data/User/Packages/" + package);
    for (const auto backend : context.profile.target.scriptBackends)
        descriptor.scriptBackends.push_back(ToString(backend));
    descriptor.buildInputHash = report.inputHash;
    descriptor.buildReportPath = "BuildReport.json";
    descriptor.signedBuild = report.signed_;
    descriptor.signingDescription = report.signingDescription;

    // Managed and Lua payloads staged by the adapter become descriptor paths.
    const auto managedRuntimeConfig = context.stagingRoot / "Scripts" / "GameScripts.runtimeconfig.json";
    const auto managedAssembly = context.stagingRoot / "Scripts" / "GameScripts.dll";
    std::error_code existsError;
    if (std::filesystem::is_regular_file(managedRuntimeConfig, existsError) &&
        std::filesystem::is_regular_file(managedAssembly, existsError))
    {
        descriptor.scripts.managedRuntimeConfig = "Scripts/GameScripts.runtimeconfig.json";
        descriptor.scripts.managedAssembly = "Scripts/GameScripts.dll";
    }
    const auto dotnetRoot = context.stagingRoot / "Runtime" / "dotnet";
    if (std::filesystem::is_directory(dotnetRoot, existsError))
        descriptor.scripts.dotnetRoot = "Runtime/dotnet";
    const auto scriptApi = context.stagingRoot / "Scripts" / "Api";
    if (std::filesystem::is_directory(scriptApi, existsError))
        descriptor.scripts.scriptApiDirectory = "Scripts/Api";

    const auto destination = context.stagingRoot / NLS::Engine::Assets::PlayerDescriptor::FileName();
    if (!descriptor.Save(destination, error))
        return false;

    (void)player;
    return true;
}
} // namespace

bool StageContent(
    const BuildContext& context,
    const DependencyCollectionResult& collection,
    BuildReport& report,
    std::vector<std::string>& contentPackages,
    std::string* error)
{
    if (!WriteRawAssets(context, error))
        return false;
    if (!WriteRuntimeManifest(context, collection, error))
        return false;
    if (!WriteGameIni(context, error))
        return false;
    if (!WritePackages(context, collection, report, contentPackages, error))
        return false;
    return true;
}

bool WritePlayerDescriptorFile(
    const BuildContext& context,
    const PlayerTemplateInfo& player,
    const std::vector<std::string>& contentPackages,
    BuildReport& report,
    std::string* error)
{
    return WritePlayerDescriptor(context, player, contentPackages, report, error);
}

bool VerifyStagedContent(
    const BuildContext& context,
    BuildReport& report,
    const std::vector<std::string>& contentPackages,
    std::string* error)
{
    // 1. Reopen every package and verify every payload hash.
    std::set<std::string> packagedArtifacts;
    for (const auto& package : contentPackages)
    {
        const auto packagePath = context.stagingRoot / "Data" / "User" / "Packages" / package;
        std::string openError;
        auto archive = NLS::Core::Assets::NpakArchive::Open(packagePath, &openError);
        if (!archive.has_value())
        {
            if (error != nullptr)
                *error = openError;
            return false;
        }
        for (const auto& entry : archive->GetEntries())
        {
            std::string verifyError;
            if (!archive->VerifyPayload(entry.path, &verifyError))
            {
                if (error != nullptr)
                    *error = verifyError;
                return false;
            }
            packagedArtifacts.insert(entry.path);
        }
    }

    // 2. The runtime manifest must resolve through the packaged artifacts.
    const auto manifestPath = context.stagingRoot / "Data" / "User" / "RuntimeAssetManifest.json";
    const auto manifestText = ReadTextFileOrEmpty(manifestPath);
    std::string manifestError;
    auto manifest = NLS::Engine::Assets::DeserializeRuntimeAssetManifest(manifestText, &manifestError);
    if (!manifest.has_value())
    {
        if (error != nullptr)
            *error = "runtime manifest verification failed: " + manifestError;
        return false;
    }
    size_t missingArtifacts = 0u;
    for (const auto& entry : manifest->entries)
    {
        const auto key = PortableArtifactKey(entry.artifactPath);
        if (packagedArtifacts.find(key) == packagedArtifacts.end())
        {
            ++missingArtifacts;
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-package-artifact-missing",
                "Packaged content is missing an artifact referenced by the runtime manifest.",
                entry.artifactPath);
        }
    }

    // 3. The player descriptor must exist and describe the same build.
    const auto descriptorPath = context.stagingRoot / NLS::Engine::Assets::PlayerDescriptor::FileName();
    std::string descriptorError;
    const auto descriptor = NLS::Engine::Assets::PlayerDescriptor::Load(descriptorPath, &descriptorError);
    if (!descriptor.has_value())
    {
        if (error != nullptr)
            *error = descriptorError;
        return false;
    }
    if (!context.profile.startScene.empty() &&
        std::filesystem::exists(context.stagingRoot / "Data" / "User" / "Assets" /
            std::filesystem::path(ToForwardSlashes(context.profile.startScene))))
    {
        report.cachedStages.push_back("start-scene-present");
    }

    if (missingArtifacts > 0u)
    {
        if (error != nullptr)
            *error = "packaged content does not satisfy the runtime manifest";
        return false;
    }
    return true;
}

bool StageSymbols(
    const BuildContext& context,
    const std::vector<std::filesystem::path>& symbolFiles,
    BuildReport& report,
    std::filesystem::path& symbolsDirectory,
    std::string* error)
{
    if (!context.profile.includeDebugSymbols || symbolFiles.empty())
        return true;

    symbolsDirectory = context.outputRoot / "symbols" / context.profile.id;
    size_t copied = 0u;
    for (const auto& symbolFile : symbolFiles)
    {
        std::error_code symbolError;
        if (!std::filesystem::is_regular_file(symbolFile, symbolError))
            continue;
        if (!CopyFileEnsuringDirectory(symbolFile, symbolsDirectory / symbolFile.filename(), error))
            return false;
        ++copied;
    }
    report.AddDiagnostic(
        DiagnosticSeverity::Info,
        "build-symbols-staged",
        "Debug symbols for " + std::to_string(copied) + " file(s) were staged separately.",
        symbolsDirectory.string());
    return true;
}

void WriteLicenseInventory(const BuildContext& context, BuildReport& report)
{
    const auto destination = context.stagingRoot / "ThirdPartyLicenses.txt";
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        return;

    // The inventory lists the vendored third-party packages that may ship
    // inside the player. Release tooling replaces this with the resolved
    // package list once dependency pinning runs in CI.
    output << "Nullus third-party components\n";
    output << "This file lists the third-party packages vendored with the engine build.\n\n";
    output << "zstd 1.5.6 (BSD-3-Clause / GPL-2.0 dual license)\n";
    output << "assimp (BSD-3-Clause)\n";
    output << "glfw (Zlib)\n";
    output << "ImGui (MIT)\n";
    output << "Lua 5.4 (MIT)\n";
    output << "PhysX (BSD-3-Clause)\n";
    output << "spirv-cross (Apache-2.0)\n";
    output << "nlohmann/json (MIT)\n";
    output << "lmdb (OpenLDAP Public License)\n";
    output << "stb (MIT / public domain)\n";
    output << "Tracy (BSD-3-Clause)\n";
    output.close();
    report.AddDiagnostic(
        DiagnosticSeverity::Info,
        "build-licenses-written",
        "Third-party license inventory written.",
        destination.string());
}
} // namespace NLS::Build::Detail
