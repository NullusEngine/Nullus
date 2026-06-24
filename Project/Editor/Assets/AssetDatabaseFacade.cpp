#include "Assets/AssetDatabaseFacade.h"

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactWriter.h"
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/AssetPath.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/ExternalAssetImporter.h"
#include "Guid.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Resources/ShaderReflectionMerge.h"
#include "Rendering/Resources/ShaderType.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"
#include "Rendering/ShaderLab/ShaderLabVariant.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace NLS::Editor::Assets
{
namespace
{
constexpr const char* kStandardPbrShaderAssetPath = "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader";
constexpr const char* kStandardPbrShaderLibraryPath = "Assets/Engine/Shaders/NullusShaderLibrary";
constexpr const char* kShaderCompilerToolchainDependencyName = "shader-compiler-toolchain";

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::vector<std::string> SplitList(const std::string& text)
{
    std::vector<std::string> values;
    std::stringstream stream(text);
    std::string value;
    while (std::getline(stream, value, ';'))
    {
        if (!value.empty())
            values.push_back(value);
    }

    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::string JoinList(std::vector<std::string> values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());

    std::string joined;
    for (const auto& value : values)
    {
        if (value.empty())
            continue;
        if (!joined.empty())
            joined.push_back(';');
        joined += value;
    }
    return joined;
}

std::filesystem::path FindBundledShaderAssetSource(const std::filesystem::path& relative)
{
    std::vector<std::filesystem::path> probes;
#if defined(NLS_ROOT_DIR)
    probes.push_back(std::filesystem::path(NLS_ROOT_DIR));
#endif
    probes.push_back(std::filesystem::current_path());
    probes.push_back(std::filesystem::absolute(std::filesystem::path(".")));

    for (auto probe : probes)
    {
        probe = probe.lexically_normal();
        const auto directCandidate = probe / relative;
        if (std::filesystem::exists(directCandidate))
            return directCandidate;

        while (!probe.empty())
        {
            const auto candidate = probe / relative;
            if (std::filesystem::exists(candidate))
                return candidate;
            const auto parent = probe.parent_path();
            if (parent == probe)
                break;
            probe = parent;
        }
    }
    return {};
}

bool CopyDirectoryRecursive(
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    std::error_code error;
    if (std::filesystem::exists(destination, error))
    {
        std::filesystem::remove_all(destination, error);
        if (error)
            return false;
    }
    std::filesystem::create_directories(destination, error);
    if (error)
        return false;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, error))
    {
        if (error)
            return false;

        const auto relative = entry.path().lexically_relative(source);
        const auto target = destination / relative;
        if (entry.is_directory(error))
        {
            std::filesystem::create_directories(target, error);
            if (error)
                return false;
            continue;
        }
        if (!entry.is_regular_file(error))
            continue;

        std::filesystem::create_directories(target.parent_path(), error);
        if (error)
            return false;

        std::filesystem::copy_file(
            entry.path(),
            target,
            std::filesystem::copy_options::overwrite_existing,
            error);
        if (error)
            return false;
    }
    return true;
}

std::string FileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return {};

    const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
    return std::to_string(size) + ":" + std::to_string(writeTimeTicks);
}

std::string ParentEditorAssetPath(const std::string& assetPath)
{
    return NormalizeEditorAssetPath(std::filesystem::path(assetPath).parent_path());
}

bool IsDependencyKindAssetReference(const NLS::Core::Assets::AssetDependencyKind kind)
{
    using NLS::Core::Assets::AssetDependencyKind;
    switch (kind)
    {
    case AssetDependencyKind::SourceAssetGuid:
    case AssetDependencyKind::ImportedArtifact:
    case AssetDependencyKind::PrefabBase:
    case AssetDependencyKind::NestedPrefab:
    case AssetDependencyKind::PrefabOverrideTarget:
        return true;
    default:
        return false;
    }
}

bool IsStampDependencyKind(const NLS::Core::Assets::AssetDependencyKind kind)
{
    using NLS::Core::Assets::AssetDependencyKind;
    return kind == AssetDependencyKind::SourceFileHash ||
        kind == AssetDependencyKind::PathToGuidMapping;
}

bool FilesHaveSameContents(
    const std::filesystem::path& lhs,
    const std::filesystem::path& rhs)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(lhs, error) ||
        !std::filesystem::is_regular_file(rhs, error))
    {
        return false;
    }

    error.clear();
    const auto lhsSize = std::filesystem::file_size(lhs, error);
    if (error)
        return false;
    const auto rhsSize = std::filesystem::file_size(rhs, error);
    if (error)
        return false;
    if (lhsSize != rhsSize)
        return false;

    std::ifstream lhsInput(lhs, std::ios::binary);
    std::ifstream rhsInput(rhs, std::ios::binary);
    if (!lhsInput || !rhsInput)
        return false;

    std::array<char, 64u * 1024u> lhsBuffer {};
    std::array<char, 64u * 1024u> rhsBuffer {};
    while (lhsInput && rhsInput)
    {
        lhsInput.read(lhsBuffer.data(), static_cast<std::streamsize>(lhsBuffer.size()));
        rhsInput.read(rhsBuffer.data(), static_cast<std::streamsize>(rhsBuffer.size()));
        if (lhsInput.gcount() != rhsInput.gcount())
            return false;
        if (!std::equal(lhsBuffer.begin(), lhsBuffer.begin() + lhsInput.gcount(), rhsBuffer.begin()))
            return false;
    }
    return lhsInput.eof() && rhsInput.eof();
}

bool DirectoriesHaveSameContents(
    const std::filesystem::path& lhs,
    const std::filesystem::path& rhs)
{
    std::error_code error;
    if (!std::filesystem::is_directory(lhs, error) ||
        !std::filesystem::is_directory(rhs, error))
    {
        return false;
    }

    std::vector<std::filesystem::path> lhsFiles;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(lhs, error))
    {
        if (error)
            return false;
        if (entry.is_regular_file(error))
            lhsFiles.push_back(entry.path().lexically_relative(lhs));
    }
    if (error)
        return false;

    std::vector<std::filesystem::path> rhsFiles;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(rhs, error))
    {
        if (error)
            return false;
        if (entry.is_regular_file(error))
            rhsFiles.push_back(entry.path().lexically_relative(rhs));
    }
    if (error)
        return false;

    std::sort(lhsFiles.begin(), lhsFiles.end());
    std::sort(rhsFiles.begin(), rhsFiles.end());
    if (lhsFiles != rhsFiles)
        return false;

    return std::all_of(
        lhsFiles.begin(),
        lhsFiles.end(),
        [&](const std::filesystem::path& relative)
        {
            return FilesHaveSameContents(lhs / relative, rhs / relative);
        });
}

std::vector<NLS::Core::Assets::AssetDependencyRecord> BuildNativeAssetManifestDependencies(
    std::string editorAssetPath,
    std::string editorMetaPath,
    const std::filesystem::path& absolutePath,
    const std::filesystem::path& metaPath,
    const NLS::Core::Assets::AssetMeta& meta,
    const std::string& editorTarget = "editor")
{
    editorAssetPath = NormalizeEditorAssetPath(std::move(editorAssetPath));
    editorMetaPath = NormalizeEditorAssetPath(std::move(editorMetaPath));
    return {
        {
            NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
            std::move(editorAssetPath),
            FileStamp(absolutePath)
        },
        {
            NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
            std::move(editorMetaPath),
            FileStamp(metaPath)
        },
        {
            NLS::Core::Assets::AssetDependencyKind::ImporterVersion,
            meta.importerId,
            std::to_string(meta.importerVersion)
        },
        {
            NLS::Core::Assets::AssetDependencyKind::BuildTarget,
            editorTarget,
            editorTarget
        }
    };
}

void AddNativeAssetManifestDependencies(
    std::vector<NLS::Core::Assets::AssetDependencyRecord>& dependencies,
    std::string editorAssetPath,
    std::string editorMetaPath,
    const std::filesystem::path& absolutePath,
    const std::filesystem::path& metaPath,
    const NLS::Core::Assets::AssetMeta& meta,
    const std::string& editorTarget = "editor")
{
    for (auto dependency : BuildNativeAssetManifestDependencies(
        std::move(editorAssetPath),
        std::move(editorMetaPath),
        absolutePath,
        metaPath,
        meta,
        editorTarget))
    {
        const auto exists = std::any_of(
            dependencies.begin(),
            dependencies.end(),
            [&dependency](const NLS::Core::Assets::AssetDependencyRecord& existing)
            {
                return existing.kind == dependency.kind &&
                    existing.value == dependency.value &&
                    existing.hashOrVersion == dependency.hashOrVersion;
            });
        if (!exists)
            dependencies.push_back(std::move(dependency));
    }
}

NLS::Core::Assets::AssetDependencyRecord BuildShaderCompilerToolchainDependency()
{
    return {
        NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
        kShaderCompilerToolchainDependencyName,
        NLS::Render::ShaderCompiler::BuildShaderCompilerToolchainDependencyFingerprint()
    };
}

bool HasDependency(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetDependencyKind kind,
    const std::string_view value,
    const std::string_view hashOrVersion)
{
    return std::any_of(
        manifest.dependencies.begin(),
        manifest.dependencies.end(),
        [kind, value, hashOrVersion](const NLS::Core::Assets::AssetDependencyRecord& dependency)
        {
            return dependency.kind == kind &&
                dependency.value == value &&
                dependency.hashOrVersion == hashOrVersion;
        });
}

uint32_t NativeArtifactSchemaVersionForSnapshotType(
    const NLS::Core::Assets::ArtifactType artifactType)
{
    switch (artifactType)
    {
    case NLS::Core::Assets::ArtifactType::Mesh:
        return 3u;
    case NLS::Core::Assets::ArtifactType::Texture:
        return 4u;
    case NLS::Core::Assets::ArtifactType::Material:
    case NLS::Core::Assets::ArtifactType::Prefab:
    case NLS::Core::Assets::ArtifactType::Shader:
    case NLS::Core::Assets::ArtifactType::Scene:
    case NLS::Core::Assets::ArtifactType::Audio:
    case NLS::Core::Assets::ArtifactType::Skeleton:
    case NLS::Core::Assets::ArtifactType::Skin:
    case NLS::Core::Assets::ArtifactType::AnimationClip:
    case NLS::Core::Assets::ArtifactType::MorphTarget:
    case NLS::Core::Assets::ArtifactType::Model:
    case NLS::Core::Assets::ArtifactType::Unknown:
    case NLS::Core::Assets::ArtifactType::Count:
        return 1u;
    }
    return 1u;
}

std::string ReadSubAssetDisplayNameFromArtifact(
    const std::string& artifactPath,
    const NLS::Core::Assets::ArtifactType artifactType)
{
    if (artifactPath.empty())
        return {};

    const auto prefix = NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
        artifactPath,
        artifactType,
        NativeArtifactSchemaVersionForSnapshotType(artifactType),
        0u,
        64u * 1024u);
    if (!prefix.has_value())
        return {};
    return prefix->metadata.displayName;
}

bool SubAssetKeyNeedsDisplayNameProbe(const std::string& subAssetKey)
{
    const auto separator = subAssetKey.find(':');
    auto name = separator == std::string::npos || separator + 1u >= subAssetKey.size()
        ? subAssetKey
        : subAssetKey.substr(separator + 1u);
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos && slash + 1u < name.size())
        name = name.substr(slash + 1u);
    return name.empty() || std::all_of(
        name.begin(),
        name.end(),
        [](const unsigned char ch)
        {
            return std::isdigit(ch) != 0;
        });
}

std::string ReadSubAssetDisplayNameForSnapshot(
    const NLS::Core::Assets::ImportedArtifact& artifact)
{
    if (!artifact.displayName.empty())
        return artifact.displayName;
    if (!SubAssetKeyNeedsDisplayNameProbe(artifact.subAssetKey))
        return {};
    return ReadSubAssetDisplayNameFromArtifact(artifact.artifactPath, artifact.artifactType);
}

bool HasCurrentExternalTextureBuildPipelineDependency(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetType assetType)
{
    if (assetType != NLS::Core::Assets::AssetType::ModelScene)
        return true;

    const bool hasTextureArtifact = std::any_of(
        manifest.subAssets.begin(),
        manifest.subAssets.end(),
        [](const NLS::Core::Assets::ImportedArtifact& artifact)
        {
            return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
        });
    if (!hasTextureArtifact)
        return true;

    return HasDependency(
        manifest,
        NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
        kExternalTextureBuildPipelineDependencyName,
        std::to_string(kExternalTexturePostprocessorVersion));
}

std::filesystem::path ResolvePhysicalArtifactFileInsideRoot(
    const std::filesystem::path& artifactRoot,
    const std::filesystem::path& rawPath)
{
    if (artifactRoot.empty() || rawPath.empty())
        return {};
    if (!NLS::Core::Assets::IsArtifactStorageFileName(rawPath.filename().generic_string()))
        return {};

    auto relativePath = rawPath.lexically_normal();
    if (relativePath.is_absolute() || relativePath.has_root_name() || relativePath.has_root_directory())
        return {};
    const auto libraryIt = std::find(relativePath.begin(), relativePath.end(), std::filesystem::path("Library"));
    if (libraryIt != relativePath.end())
    {
        std::filesystem::path libraryRelative;
        for (auto it = libraryIt; it != relativePath.end(); ++it)
            libraryRelative /= *it;
        relativePath = std::move(libraryRelative);
    }

    const auto startsWithLibrary = !relativePath.empty() &&
        *relativePath.begin() == std::filesystem::path("Library");
    const auto artifactsPrefix = std::filesystem::path("Library") / "Artifacts";
    std::filesystem::path candidate;
    auto prefixIt = artifactsPrefix.begin();
    auto pathIt = relativePath.begin();
    for (; prefixIt != artifactsPrefix.end() && pathIt != relativePath.end() && *prefixIt == *pathIt; ++prefixIt, ++pathIt)
    {
    }
    if (prefixIt == artifactsPrefix.end())
    {
        std::filesystem::path artifactRelative;
        for (; pathIt != relativePath.end(); ++pathIt)
            artifactRelative /= *pathIt;
        candidate = NLS::Core::Assets::NormalizeAssetPath(artifactRoot / artifactRelative);
    }
    else if (relativePath.parent_path().empty())
    {
        candidate = NLS::Core::Assets::NormalizeAssetPath(artifactRoot / relativePath);
    }
    else if (!startsWithLibrary &&
        NLS::Core::Assets::IsContentStorageArtifactPath(relativePath.generic_string()))
    {
        auto pathIt = relativePath.begin();
        if (pathIt != relativePath.end() && *pathIt == std::filesystem::path("Artifacts"))
            ++pathIt;

        std::filesystem::path artifactRelative;
        for (; pathIt != relativePath.end(); ++pathIt)
            artifactRelative /= *pathIt;

        candidate = NLS::Core::Assets::NormalizeAssetPath(artifactRoot / artifactRelative);
    }
    else
    {
        return {};
    }
    if (candidate.empty() ||
        !IsPhysicalRegularFileInsideEditorAssetRoot(candidate, artifactRoot))
    {
        return {};
    }
    return candidate;
}

bool HasCurrentShaderCompilerToolchainDependency(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetType assetType)
{
    if (assetType != NLS::Core::Assets::AssetType::Shader)
        return true;

    const auto dependency = BuildShaderCompilerToolchainDependency();
    return HasDependency(
        manifest,
        dependency.kind,
        dependency.value,
        dependency.hashOrVersion);
}

std::filesystem::path ResolvePhysicalArtifactFileFromLibraryPath(
    const EditorAssetRoot& root,
    const std::filesystem::path& rawPath)
{
    const auto artifactRoot = GetEditorAssetRootLibraryPath(root) / "Artifacts";
    auto artifactPath = ResolvePhysicalArtifactFileInsideRoot(artifactRoot, rawPath);
    if (!artifactPath.empty() || rawPath.is_absolute())
        return artifactPath;

    return ResolvePhysicalArtifactFileInsideRoot(
        artifactRoot,
        GetEditorAssetRootLibraryPath(root).parent_path() / rawPath);
}

void AddUniqueDependency(
    std::vector<NLS::Core::Assets::AssetDependencyRecord>& dependencies,
    NLS::Core::Assets::AssetDependencyRecord dependency)
{
    const auto exists = std::any_of(
        dependencies.begin(),
        dependencies.end(),
        [&dependency](const NLS::Core::Assets::AssetDependencyRecord& existing)
        {
            return existing.kind == dependency.kind &&
                existing.value == dependency.value &&
                existing.hashOrVersion == dependency.hashOrVersion;
        });
    if (!exists)
        dependencies.push_back(std::move(dependency));
}

std::filesystem::path ResolveManifestStampDependencyPath(
    const std::vector<EditorAssetRoot>& roots,
    const NLS::Core::Assets::SourceAssetRecord& record,
    const std::string& dependencyValue,
    const bool preferEditorAssetPath)
{
    const auto* root = FindEditorAssetRootForAbsolutePath(roots, record.absolutePath);
    if (!root)
        return {};

    const auto rootRelativePath = ResolveEditorManifestDependencyPath(root->path, dependencyValue);
    std::error_code rootRelativeError;
    if (rootRelativePath.has_value() &&
        std::filesystem::is_regular_file(*rootRelativePath, rootRelativeError) &&
        IsPathInsideEditorAssetRoot(*rootRelativePath, root->path) &&
        IsPhysicalPathInsideEditorAssetRoot(*rootRelativePath, root->path))
    {
        return *rootRelativePath;
    }

    if (preferEditorAssetPath)
    {
        auto dependencyPath = ResolveEditorAssetPath(roots, dependencyValue);
        if (!dependencyPath.empty())
            return dependencyPath;
    }

    if (!preferEditorAssetPath)
    {
        auto dependencyPath = ResolveEditorAssetPath(roots, dependencyValue);
        if (!dependencyPath.empty())
            return dependencyPath;
    }

    return {};
}

bool HasErrors(const NLS::Core::Assets::AssetDiagnostics& diagnostics)
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.severity == NLS::Core::Assets::AssetDiagnosticSeverity::Error;
        });
}

bool ContainsDiagnosticCode(
    const NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const std::string& code)
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [&code](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.code == code;
        });
}

ImportJobTerminalStatus TerminalStatusForImportFailure(
    const NLS::Core::Assets::AssetDiagnostics& diagnostics)
{
    return ContainsDiagnosticCode(diagnostics, "artifact-write-cancelled")
        ? ImportJobTerminalStatus::Cancelled
        : ImportJobTerminalStatus::Failed;
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};

    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};

    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

bool HasEntryPointToken(const std::string& source, std::string_view entryPoint)
{
    return !source.empty() && source.find(std::string(entryPoint)) != std::string::npos;
}

uint64_t StableShaderLabCacheHash(const std::string& value)
{
    uint64_t hash = 1469598103934665603ull;
    for (const auto character : value)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::filesystem::path ShaderLabImportSourcePath(
    const std::filesystem::path& sourcePath,
    const EditorAssetRoot* assetRoot,
    const std::string& subAssetKey,
    const std::string& editorAssetPath)
{
    const auto cacheRoot = assetRoot
        ? GetEditorAssetRootLibraryPath(*assetRoot) / "ShaderCache" / "ImportedShaderLab"
        : std::filesystem::temp_directory_path() / "NullusShaderLabImport";
    std::string fileName = sourcePath.stem().generic_string();
    for (char& character : fileName)
    {
        if (!std::isalnum(static_cast<unsigned char>(character)) &&
            character != '_' &&
            character != '-')
        {
            character = '_';
        }
    }
    return cacheRoot /
        (fileName + "_" + std::to_string(StableShaderLabCacheHash(editorAssetPath + "|" + subAssetKey)) + ".hlsl");
}

std::filesystem::path PrepareShaderImportCompileSource(
    const std::filesystem::path& sourcePath,
    const std::string& sourceText,
    const std::string& subAssetKey,
    const EditorAssetRoot* assetRoot,
    std::string& compileSourceText,
    std::string& vertexEntry,
    std::string& fragmentEntry,
    std::string& computeEntry,
    std::optional<NLS::Render::ShaderLab::ShaderLabPassState>& shaderLabPassState,
    std::string& shaderLabImportDiagnostics)
{
    compileSourceText = sourceText;
    vertexEntry = HasEntryPointToken(sourceText, "VSMain") ? "VSMain" : "";
    fragmentEntry = HasEntryPointToken(sourceText, "PSMain") ? "PSMain" : "";
    computeEntry = HasEntryPointToken(sourceText, "CSMain") ? "CSMain" : "";
    shaderLabPassState.reset();
    shaderLabImportDiagnostics.clear();

    if (ToLower(sourcePath.extension().generic_string()) != ".shader")
        return sourcePath;

    compileSourceText.clear();
    vertexEntry.clear();
    fragmentEntry.clear();
    computeEntry.clear();

    const auto parsed = NLS::Render::ShaderLab::ParseShaderLabSource(
        sourceText,
        sourcePath.generic_string());
    if (!parsed.Succeeded())
    {
        shaderLabImportDiagnostics = parsed.DiagnosticsToString();
        return {};
    }

    const NLS::Render::ShaderLab::ShaderLabPassDesc* pass = nullptr;
    for (const auto& subShader : parsed.asset.subShaders)
    {
        if (!subShader.passes.empty())
        {
            pass = &subShader.passes.front();
            break;
        }
    }
    if (!pass)
    {
        shaderLabImportDiagnostics = sourcePath.generic_string() + ": ShaderLab asset has no Pass to import.";
        return {};
    }

    compileSourceText = NLS::Render::ShaderLab::BuildShaderLabHlslForCompile(*pass);
    vertexEntry = pass->vertexEntry;
    fragmentEntry = pass->fragmentEntry;
    computeEntry = pass->computeEntry;
    shaderLabPassState = pass->state;

    const auto compilePath = ShaderLabImportSourcePath(
        sourcePath,
        assetRoot,
        subAssetKey,
        sourcePath.lexically_normal().generic_string());
    std::filesystem::create_directories(compilePath.parent_path());
    std::ofstream output(compilePath, std::ios::binary | std::ios::trunc);
    output << compileSourceText;
    if (output)
        return compilePath;

    shaderLabImportDiagnostics = compilePath.generic_string() + ": failed to write generated ShaderLab HLSL.";
    return {};
}

NLS::Render::Assets::ShaderArtifactStage MakeFailedShaderLabImportStage(std::string diagnostics)
{
    NLS::Render::Assets::ShaderArtifactStage stage;
    stage.stage = NLS::Render::ShaderCompiler::ShaderStage::Vertex;
    stage.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::Unknown;
    stage.output.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Failed;
    stage.output.diagnostics = std::move(diagnostics);
    return stage;
}

struct ImportedShaderArtifactPayload
{
    std::string subAssetKey;
    std::string displayName;
    NLS::Render::Assets::ShaderArtifact artifact;
};

struct ImportedShaderLabArtifactPayloads
{
    std::vector<ImportedShaderArtifactPayload> payloads;
    std::string diagnostic;

    [[nodiscard]] bool Succeeded() const
    {
        return diagnostic.empty();
    }
};

std::string ShaderCacheDatabasePathForAssetRoot(const EditorAssetRoot* root)
{
    if (!root)
        return {};
    return (GetEditorAssetRootLibraryPath(*root) / "ShaderCache" / "ShaderCache.tsv").string();
}

NLS::Render::ShaderCompiler::ShaderCompilationInput MakeShaderCompilationInput(
    const std::filesystem::path& sourcePath,
    const NLS::Render::ShaderCompiler::ShaderStage stage,
    const NLS::Render::ShaderCompiler::ShaderTargetPlatform targetPlatform,
    std::string entryPoint,
    std::string targetProfile,
    const std::filesystem::path& originalSourcePath = {},
    const NLS::Render::ShaderLab::ShaderLabKeywordSet& keywords = {})
{
    NLS::Render::ShaderCompiler::ShaderCompileOptions options;
    options.sourceLanguage = NLS::Render::ShaderCompiler::ShaderSourceLanguage::HLSL;
    options.targetPlatform = targetPlatform;
    options.entryPoint = std::move(entryPoint);
    options.targetProfile = std::move(targetProfile);
    options.includeDirectories.push_back(sourcePath.parent_path().string());
    auto appendIncludeDirectory =
        [&options](const std::filesystem::path& directory)
    {
        if (directory.empty())
            return;
        const auto normalized = directory.lexically_normal().string();
        if (normalized.empty())
            return;
        if (std::find(
                options.includeDirectories.begin(),
                options.includeDirectories.end(),
                normalized) == options.includeDirectories.end())
        {
            options.includeDirectories.push_back(normalized);
        }
    };
    if (!originalSourcePath.empty())
    {
        const auto originalParent = originalSourcePath.parent_path();
        appendIncludeDirectory(originalParent);
        appendIncludeDirectory(originalParent.parent_path());
    }
    options.macros = NLS::Render::ShaderLab::BuildShaderLabKeywordMacros(keywords);
    return {sourcePath.string(), stage, std::move(options)};
}

void NormalizeShaderImportOutput(NLS::Render::ShaderCompiler::ShaderCompilationOutput& output)
{
    if (output.bytecode.empty() && !output.artifactPath.empty())
        output.bytecode = ReadBinaryFile(output.artifactPath);
    if (!output.bytecode.empty() && output.status != NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
        output.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
}

NLS::Render::Assets::ShaderArtifactStage MakeShaderArtifactStage(
    const NLS::Render::ShaderCompiler::ShaderCompilationInput& input,
    NLS::Render::ShaderCompiler::ShaderCompilationOutput output,
    const uint64_t keywordHash = 0u)
{
    NormalizeShaderImportOutput(output);

    NLS::Render::Assets::ShaderArtifactStage stage;
    stage.stage = input.stage;
    stage.targetPlatform = input.options.targetPlatform;
    stage.entryPoint = input.options.entryPoint;
    stage.targetProfile = input.options.targetProfile;
    stage.keywordHash = keywordHash;
    stage.output = std::move(output);
    return stage;
}

bool ReflectImportedShaderStagesDxilFirst(
    NLS::Render::ShaderCompiler::ShaderCompiler& compiler,
    const std::vector<NLS::Render::ShaderCompiler::ShaderCompilationInput>& inputs,
    const std::vector<NLS::Render::Assets::ShaderArtifactStage>& stages,
    NLS::Render::Resources::ShaderReflection& reflection,
    std::string* diagnostic)
{
    auto collectReflectionInputs =
        [&inputs, &stages](const NLS::Render::ShaderCompiler::ShaderTargetPlatform platform)
    {
        std::vector<NLS::Render::ShaderCompiler::ShaderReflectionInput> reflectionInputs;
        reflectionInputs.reserve(inputs.size());
        for (size_t index = 0u; index < stages.size() && index < inputs.size(); ++index)
        {
            const auto& stage = stages[index];
            if (stage.targetPlatform != platform ||
                stage.output.status != NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded ||
                stage.output.bytecode.empty())
            {
                continue;
            }
            reflectionInputs.push_back({inputs[index], stage.output});
        }
        return reflectionInputs;
    };

    auto reflectStages =
        [&compiler](const std::vector<NLS::Render::ShaderCompiler::ShaderReflectionInput>& reflectionInputs)
    {
        return compiler.ReflectBatch(reflectionInputs);
    };

    const auto dxilReflections =
        reflectStages(collectReflectionInputs(NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL));
    const auto spirvReflections =
        reflectStages(collectReflectionInputs(NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV));
    return NLS::Render::Resources::TryMergePreferredShaderReflectionOrFallback(
        dxilReflections,
        spirvReflections,
        reflection,
        diagnostic);
}

bool HasFailedShaderArtifactStage(const NLS::Render::Assets::ShaderArtifact& artifact)
{
    return std::any_of(
        artifact.stages.begin(),
        artifact.stages.end(),
        [](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.output.status != NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded ||
                stage.output.bytecode.empty();
        });
}

std::string SanitizeShaderLabPassKey(std::string value)
{
    if (value.empty())
        value = "Pass";
    for (auto& character : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(character)) &&
            character != '_' &&
            character != '-')
        {
            character = '_';
        }
    }
    return value;
}

std::vector<NLS::Render::ShaderLab::ShaderLabKeywordSet> BuildShaderLabImportKeywordVariants(
    const NLS::Render::ShaderLab::ShaderLabPassDesc& pass)
{
    using NLS::Render::ShaderLab::ShaderLabKeywordSet;
    using NLS::Render::ShaderLab::ShaderLabKeywordPragma;

    std::vector<ShaderLabKeywordSet> variants;
    variants.emplace_back();

    auto appendPragma = [&variants](const ShaderLabKeywordPragma& pragma, const bool includeOffOption)
    {
        std::vector<std::string> options;
        if (includeOffOption)
            options.emplace_back();
        for (const auto& keyword : pragma.keywords)
        {
            if (keyword == "_")
            {
                if (std::find(options.begin(), options.end(), std::string {}) == options.end())
                    options.emplace_back();
                continue;
            }
            if (!keyword.empty() && std::find(options.begin(), options.end(), keyword) == options.end())
                options.push_back(keyword);
        }
        if (options.empty())
            return;

        std::vector<ShaderLabKeywordSet> expanded;
        expanded.reserve(variants.size() * options.size());
        for (const auto& base : variants)
        {
            for (const auto& keyword : options)
            {
                auto candidate = base;
                if (!keyword.empty())
                    candidate.Enable(keyword);
                expanded.push_back(std::move(candidate));
            }
        }
        variants = std::move(expanded);
    };

    for (const auto& pragma : pass.multiCompiles)
        appendPragma(pragma, false);

    std::sort(
        variants.begin(),
        variants.end(),
        [](const ShaderLabKeywordSet& lhs, const ShaderLabKeywordSet& rhs)
        {
            return lhs.ToVector() < rhs.ToVector();
        });
    variants.erase(
        std::unique(
            variants.begin(),
            variants.end(),
            [](const ShaderLabKeywordSet& lhs, const ShaderLabKeywordSet& rhs)
            {
                return lhs.ToVector() == rhs.ToVector();
            }),
        variants.end());
    return variants;
}

NLS::Render::Assets::ShaderArtifact CompileShaderLabPassArtifact(
    const std::filesystem::path& sourcePath,
    const std::string& editorAssetPath,
    const std::string& subAssetKey,
    const EditorAssetRoot* assetRoot,
    const NLS::Render::ShaderLab::ShaderLabPassDesc& pass)
{
    using NLS::Render::ShaderCompiler::ShaderStage;
    using NLS::Render::ShaderCompiler::ShaderTargetPlatform;

    NLS::Render::Assets::ShaderArtifact artifact;
    artifact.sourcePath = editorAssetPath;
    artifact.subAssetKey = subAssetKey;
    artifact.targetPlatform = "editor";
    if (const auto lightMode = pass.tags.values.find("LightMode");
        lightMode != pass.tags.values.end())
    {
        artifact.shaderLabLightMode = lightMode->second;
    }
    artifact.shaderLabPassState = pass.state;

    const auto compileSourceText = NLS::Render::ShaderLab::BuildShaderLabHlslForCompile(pass);
    auto compilePath = ShaderLabImportSourcePath(sourcePath, assetRoot, subAssetKey, editorAssetPath);
    compilePath = compilePath.parent_path() /
        (compilePath.stem().generic_string() + "_" + SanitizeShaderLabPassKey(pass.name) + ".hlsl");
    std::filesystem::create_directories(compilePath.parent_path());
    {
        std::ofstream output(compilePath, std::ios::binary | std::ios::trunc);
        output << compileSourceText;
        if (!output)
        {
            artifact.stages.push_back(MakeFailedShaderLabImportStage(
                compilePath.generic_string() + ": failed to write generated ShaderLab HLSL."));
            return artifact;
        }
    }

    auto buildInputs = [&](const NLS::Render::ShaderLab::ShaderLabKeywordSet& keywords)
    {
        std::vector<NLS::Render::ShaderCompiler::ShaderCompilationInput> inputs;
        if (!pass.vertexEntry.empty())
        {
            inputs.push_back(MakeShaderCompilationInput(compilePath, ShaderStage::Vertex, ShaderTargetPlatform::DXIL, pass.vertexEntry, "vs_6_0", sourcePath, keywords));
            inputs.push_back(MakeShaderCompilationInput(compilePath, ShaderStage::Vertex, ShaderTargetPlatform::SPIRV, pass.vertexEntry, "vs_6_0", sourcePath, keywords));
        }
        if (!pass.fragmentEntry.empty())
        {
            inputs.push_back(MakeShaderCompilationInput(compilePath, ShaderStage::Pixel, ShaderTargetPlatform::DXIL, pass.fragmentEntry, "ps_6_0", sourcePath, keywords));
            inputs.push_back(MakeShaderCompilationInput(compilePath, ShaderStage::Pixel, ShaderTargetPlatform::SPIRV, pass.fragmentEntry, "ps_6_0", sourcePath, keywords));
        }
        if (!pass.computeEntry.empty())
        {
            inputs.push_back(MakeShaderCompilationInput(compilePath, ShaderStage::Compute, ShaderTargetPlatform::DXIL, pass.computeEntry, "cs_6_0", sourcePath, keywords));
            inputs.push_back(MakeShaderCompilationInput(compilePath, ShaderStage::Compute, ShaderTargetPlatform::SPIRV, pass.computeEntry, "cs_6_0", sourcePath, keywords));
        }
        return inputs;
    };

    NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    compiler.SetCacheDatabasePath(ShaderCacheDatabasePathForAssetRoot(assetRoot));

    const auto keywordVariants = BuildShaderLabImportKeywordVariants(pass);
    const auto defaultVariant = std::find_if(
        keywordVariants.begin(),
        keywordVariants.end(),
        [](const NLS::Render::ShaderLab::ShaderLabKeywordSet& keywords)
        {
            return keywords.Hash() == 0u;
        });
    const NLS::Render::ShaderLab::ShaderLabKeywordSet defaultKeywords =
        defaultVariant != keywordVariants.end()
            ? *defaultVariant
            : NLS::Render::ShaderLab::ShaderLabKeywordSet {};
    const auto defaultInputs = buildInputs(defaultKeywords);
    const auto defaultOutputs = compiler.CompileBatch(defaultInputs);
    artifact.stages.reserve(defaultOutputs.size() * std::max<size_t>(keywordVariants.size(), 1u));
    for (size_t index = 0u; index < defaultOutputs.size() && index < defaultInputs.size(); ++index)
        artifact.stages.push_back(MakeShaderArtifactStage(defaultInputs[index], defaultOutputs[index], defaultKeywords.Hash()));

    std::vector<NLS::Render::ShaderCompiler::ShaderCompilationInput> reflectionInputs = defaultInputs;

    for (const auto& keywords : keywordVariants)
    {
        if (keywords.Hash() == defaultKeywords.Hash())
            continue;
        const auto variantInputs = buildInputs(keywords);
        const auto variantOutputs = compiler.CompileBatch(variantInputs);
        for (size_t index = 0u; index < variantOutputs.size() && index < variantInputs.size(); ++index)
            artifact.stages.push_back(MakeShaderArtifactStage(variantInputs[index], variantOutputs[index], keywords.Hash()));
    }

    std::string reflectionDiagnostics;
    if (!ReflectImportedShaderStagesDxilFirst(compiler, reflectionInputs, artifact.stages, artifact.reflection, &reflectionDiagnostics))
    {
        artifact.reflection = {};
        artifact.stages.push_back(MakeFailedShaderLabImportStage(
            reflectionDiagnostics.empty()
                ? "Shader reflection merge failed."
                : reflectionDiagnostics));
    }

    NLS::Render::Assets::AppendGlslShaderArtifactStages(artifact);

    return artifact;
}

ImportedShaderLabArtifactPayloads ImportShaderLabArtifactPayloads(
    const std::filesystem::path& sourcePath,
    const std::string& editorAssetPath,
    const std::string& baseSubAssetKey,
    const EditorAssetRoot* assetRoot)
{
    ImportedShaderLabArtifactPayloads result;
    const auto sourceText = ReadTextFile(sourcePath);
    const auto parsed = NLS::Render::ShaderLab::ParseShaderLabSource(
        sourceText,
        sourcePath.generic_string());
    if (!parsed.Succeeded())
    {
        result.diagnostic = parsed.DiagnosticsToString();
        return result;
    }

    std::vector<const NLS::Render::ShaderLab::ShaderLabPassDesc*> passes;
    const NLS::Render::ShaderLab::ShaderLabPassDesc* primaryPass = nullptr;
    for (const auto& subShader : parsed.asset.subShaders)
    {
        for (const auto& pass : subShader.passes)
        {
            const auto lightMode = pass.tags.values.find("LightMode");
            if (lightMode == pass.tags.values.end() || lightMode->second.empty())
                continue;
            passes.push_back(&pass);
            if (primaryPass == nullptr)
            {
                primaryPass = &pass;
            }
            else if (lightMode->second == "Forward")
            {
                const auto primaryLightMode = primaryPass->tags.values.find("LightMode");
                if (primaryLightMode == primaryPass->tags.values.end() ||
                    primaryLightMode->second != "Forward")
                {
                    primaryPass = &pass;
                }
            }
        }
    }

    if (primaryPass == nullptr)
    {
        result.diagnostic = sourcePath.generic_string() + ": ShaderLab asset has no LightMode Pass to import.";
        return result;
    }

    size_t passOrdinal = 0u;
    auto appendPass = [&](const NLS::Render::ShaderLab::ShaderLabPassDesc& pass)
    {
        const auto lightMode = pass.tags.values.find("LightMode");
        const auto displayName = lightMode != pass.tags.values.end() && !lightMode->second.empty()
            ? lightMode->second
            : (!pass.name.empty() ? pass.name : "Pass");
        const auto subAssetKey = &pass == primaryPass
            ? baseSubAssetKey
            : baseSubAssetKey + "/" +
                SanitizeShaderLabPassKey(pass.name.empty() ? displayName : pass.name) +
                "#" + std::to_string(passOrdinal);

        ImportedShaderArtifactPayload payload;
        payload.subAssetKey = subAssetKey;
        payload.displayName = sourcePath.stem().generic_string() + " " + displayName;
        payload.artifact = CompileShaderLabPassArtifact(
            sourcePath,
            editorAssetPath,
            subAssetKey,
            assetRoot,
            pass);
        if (!NLS::Render::Assets::HasUsableShaderArtifactStage(payload.artifact) ||
            HasFailedShaderArtifactStage(payload.artifact))
        {
            std::ostringstream diagnostic;
            diagnostic << sourcePath.generic_string() << ": ShaderLab pass \"" << displayName
                << "\" did not produce any usable shader stage.";
            for (const auto& stage : payload.artifact.stages)
            {
                if (!stage.output.diagnostics.empty())
                    diagnostic << "\n" << stage.output.diagnostics;
            }
            result.payloads.clear();
            result.diagnostic = diagnostic.str();
            return;
        }
        result.payloads.push_back(std::move(payload));
        ++passOrdinal;
    };

    appendPass(*primaryPass);
    if (!result.Succeeded())
        return result;
    for (const auto* pass : passes)
    {
        if (pass != primaryPass)
        {
            appendPass(*pass);
            if (!result.Succeeded())
                return result;
        }
    }
    return result;
}

NLS::Render::Assets::ShaderArtifact ImportShaderArtifactPayload(
    const std::filesystem::path& sourcePath,
    const std::string& editorAssetPath,
    const std::string& subAssetKey,
    const EditorAssetRoot* assetRoot)
{
    using NLS::Render::ShaderCompiler::ShaderStage;
    using NLS::Render::ShaderCompiler::ShaderTargetPlatform;

    NLS::Render::Assets::ShaderArtifact artifact;
    artifact.sourcePath = editorAssetPath;
    artifact.subAssetKey = subAssetKey;
    artifact.targetPlatform = "editor";

    const auto sourceText = ReadTextFile(sourcePath);
    std::string compileSourceText;
    std::string vertexEntry;
    std::string fragmentEntry;
    std::string computeEntry;
    std::optional<NLS::Render::ShaderLab::ShaderLabPassState> shaderLabPassState;
    std::string shaderLabImportDiagnostics;
    const auto compileSourcePath = PrepareShaderImportCompileSource(
        sourcePath,
        sourceText,
        subAssetKey,
        assetRoot,
        compileSourceText,
        vertexEntry,
        fragmentEntry,
        computeEntry,
        shaderLabPassState,
        shaderLabImportDiagnostics);
    artifact.shaderLabPassState = shaderLabPassState;
    if (compileSourcePath.empty() && !shaderLabImportDiagnostics.empty())
    {
        artifact.stages.push_back(MakeFailedShaderLabImportStage(std::move(shaderLabImportDiagnostics)));
        return artifact;
    }

    std::vector<NLS::Render::ShaderCompiler::ShaderCompilationInput> inputs;
    if (!vertexEntry.empty())
    {
        inputs.push_back(MakeShaderCompilationInput(compileSourcePath, ShaderStage::Vertex, ShaderTargetPlatform::DXIL, vertexEntry, "vs_6_0", sourcePath));
        inputs.push_back(MakeShaderCompilationInput(compileSourcePath, ShaderStage::Vertex, ShaderTargetPlatform::SPIRV, vertexEntry, "vs_6_0", sourcePath));
    }
    if (!fragmentEntry.empty())
    {
        inputs.push_back(MakeShaderCompilationInput(compileSourcePath, ShaderStage::Pixel, ShaderTargetPlatform::DXIL, fragmentEntry, "ps_6_0", sourcePath));
        inputs.push_back(MakeShaderCompilationInput(compileSourcePath, ShaderStage::Pixel, ShaderTargetPlatform::SPIRV, fragmentEntry, "ps_6_0", sourcePath));
    }
    if (!computeEntry.empty())
    {
        inputs.push_back(MakeShaderCompilationInput(compileSourcePath, ShaderStage::Compute, ShaderTargetPlatform::DXIL, computeEntry, "cs_6_0", sourcePath));
        inputs.push_back(MakeShaderCompilationInput(compileSourcePath, ShaderStage::Compute, ShaderTargetPlatform::SPIRV, computeEntry, "cs_6_0", sourcePath));
    }

    NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    compiler.SetCacheDatabasePath(ShaderCacheDatabasePathForAssetRoot(assetRoot));
    const auto outputs = compiler.CompileBatch(inputs);
    artifact.stages.reserve(outputs.size());
    for (size_t index = 0u; index < outputs.size() && index < inputs.size(); ++index)
        artifact.stages.push_back(MakeShaderArtifactStage(inputs[index], outputs[index]));
    NLS::Render::Assets::AppendGlslShaderArtifactStages(artifact);

    std::string reflectionDiagnostics;
    if (!ReflectImportedShaderStagesDxilFirst(compiler, inputs, artifact.stages, artifact.reflection, &reflectionDiagnostics))
    {
        artifact.reflection = {};
        artifact.stages.push_back(MakeFailedShaderLabImportStage(
            reflectionDiagnostics.empty()
                ? "Shader reflection merge failed."
                : reflectionDiagnostics));
    }

    return artifact;
}

std::optional<NLS::Core::Assets::AssetDependencyRecord> MakeShaderSourceFileDependency(
    const std::vector<EditorAssetRoot>& roots,
    const std::filesystem::path& ownerPath,
    const std::string& dependencyPathText)
{
    if (dependencyPathText.empty() || dependencyPathText.rfind(":", 0u) == 0u)
        return std::nullopt;

    auto dependencyPath = std::filesystem::path(dependencyPathText);
    if (dependencyPath.is_relative())
        dependencyPath = ownerPath.parent_path() / dependencyPath;
    dependencyPath = dependencyPath.lexically_normal();

    const auto* root = FindEditorAssetRootForAbsolutePath(roots, dependencyPath);
    if (!root ||
        !IsPathInsideEditorAssetRoot(dependencyPath, root->path) ||
        !IsPhysicalPathInsideEditorAssetRoot(dependencyPath, root->path))
    {
        return std::nullopt;
    }

    const auto relativePath = dependencyPath.lexically_relative(root->path);
    if (relativePath.empty() || relativePath.is_absolute())
        return std::nullopt;

    const auto stamp = FileStamp(dependencyPath);
    if (stamp.empty())
        return std::nullopt;

    auto editorPath = root->mountPath.empty()
        ? relativePath
        : root->mountPath / relativePath;
    return NLS::Core::Assets::AssetDependencyRecord {
        NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
        NormalizeEditorAssetPath(editorPath),
        stamp
    };
}

void AddShaderStageSourceDependencies(
    std::vector<NLS::Core::Assets::AssetDependencyRecord>& dependencies,
    const std::vector<EditorAssetRoot>& roots,
    const std::filesystem::path& ownerPath,
    const NLS::Render::Assets::ShaderArtifact& artifact)
{
    for (const auto& stage : artifact.stages)
    {
        for (const auto& dependencyPath : stage.output.dependencyPaths)
        {
            auto dependency = MakeShaderSourceFileDependency(roots, ownerPath, dependencyPath);
            if (dependency.has_value())
                AddUniqueDependency(dependencies, std::move(*dependency));
        }
    }
}

std::optional<std::string> FindImportedShaderArtifactResourcePath(
    const std::unordered_map<NLS::Core::Assets::AssetId, NLS::Core::Assets::ArtifactManifest>& manifestsBySource,
    const std::unordered_map<NLS::Core::Assets::AssetId, std::string>& editorPathById,
    const std::string& shaderAssetPath)
{
    const auto normalizedShaderAssetPath = NormalizeEditorAssetPath(shaderAssetPath);
    for (const auto& [assetId, editorAssetPath] : editorPathById)
    {
        if (NormalizeEditorAssetPath(editorAssetPath) != normalizedShaderAssetPath)
            continue;

        const auto manifest = manifestsBySource.find(assetId);
        if (manifest == manifestsBySource.end())
            return std::nullopt;

        const auto* primary = manifest->second.FindPrimaryArtifact();
        if (!primary || primary->artifactType != NLS::Core::Assets::ArtifactType::Shader)
            return std::nullopt;

        const auto normalizedArtifactPath = NLS::Core::Assets::NormalizeAssetPath(primary->artifactPath).generic_string();
        const auto libraryOffset = normalizedArtifactPath.find("Library/Artifacts/");
        return libraryOffset == std::string::npos
            ? normalizedArtifactPath
            : normalizedArtifactPath.substr(libraryOffset);
    }

    return std::nullopt;
}

bool IsSingleEditorAssetName(const std::string& name)
{
    if (name.empty())
        return false;

    const auto path = std::filesystem::path(name);
    if (path.is_absolute() || path.has_parent_path() || path.filename().generic_string() != name)
        return false;

    for (const auto& part : path)
    {
        if (part == "." || part == "..")
            return false;
    }
    return true;
}

std::mutex& GetArtifactDatabaseMutex(const std::filesystem::path& path)
{
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::unique_ptr<std::mutex>> mutexByPath;

    std::lock_guard registryLock(registryMutex);
    auto& mutex = mutexByPath[path.lexically_normal().generic_string()];
    if (!mutex)
        mutex = std::make_unique<std::mutex>();
    return *mutex;
}

std::mutex& GetArtifactPublishMutex(const std::filesystem::path& artifactRoot)
{
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::unique_ptr<std::mutex>> mutexByPath;

    std::lock_guard registryLock(registryMutex);
    auto& mutex = mutexByPath[artifactRoot.lexically_normal().generic_string()];
    if (!mutex)
        mutex = std::make_unique<std::mutex>();
    return *mutex;
}

struct SearchFilter
{
    std::string name;
    std::string type;
    std::string label;
};

class ArtifactRootRollback
{
public:
    explicit ArtifactRootRollback(std::filesystem::path artifactRoot)
        : m_artifactRoot(std::move(artifactRoot))
    {
        if (!m_artifactRoot.empty())
        {
            m_backupRoot = m_artifactRoot;
            m_backupRoot += ".publish-rollback";
        }
    }

    bool Prepare()
    {
        if (m_artifactRoot.empty())
            return false;
        std::error_code error;
        std::filesystem::create_directories(m_artifactRoot, error);
        return !error;
    }

    bool Restore(std::error_code* restoreError = nullptr)
    {
        if (restoreError)
            *restoreError = {};
        return true;
    }

    void Commit()
    {
        std::error_code error;
        std::filesystem::remove_all(m_backupRoot, error);
        m_hasBackup = false;
    }

private:
    std::filesystem::path m_artifactRoot;
    std::filesystem::path m_backupRoot;
    bool m_hasBackup = false;
};

void ReportArtifactRollbackRestoreFailure(
    NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const std::filesystem::path& artifactRoot,
    const std::error_code& error)
{
    NLS::Core::Assets::AssetDiagnostic diagnostic;
    diagnostic.severity = NLS::Core::Assets::AssetDiagnosticSeverity::Error;
    diagnostic.code = "assetdatabase-artifact-rollback-restore-failed";
    diagnostic.path = artifactRoot;
    diagnostic.message = "Previous imported artifact root could not be restored after failed import: " + error.message();
    diagnostics.push_back(std::move(diagnostic));
}

SearchFilter ParseSearchFilter(const std::string& filter)
{
    SearchFilter parsed;
    std::stringstream stream(filter);
    std::string token;
    while (stream >> token)
    {
        const auto colon = token.find(':');
        if (colon == std::string::npos)
        {
            parsed.name = ToLower(token);
            continue;
        }

        const auto key = ToLower(token.substr(0u, colon));
        const auto value = ToLower(token.substr(colon + 1u));
        if (key == "name")
            parsed.name = value;
        else if (key == "type")
            parsed.type = value;
        else if (key == "label")
            parsed.label = value;
    }
    return parsed;
}

bool IsPathInSearchFolders(
    const std::string& assetPath,
    const std::vector<std::string>& searchInFolders)
{
    if (searchInFolders.empty())
        return true;

    for (const auto& folder : searchInFolders)
    {
        const auto normalizedFolder = NormalizeEditorAssetPath(folder);
        if (assetPath == normalizedFolder || assetPath.find(normalizedFolder + "/") == 0u)
            return true;
    }
    return false;
}

bool ContainsLower(std::string value, const std::string& needle)
{
    return ToLower(std::move(value)).find(needle) != std::string::npos;
}

std::string ToArtifactTypeKey(const NLS::Core::Assets::ArtifactType type)
{
    using NLS::Core::Assets::ArtifactType;
    switch (type)
    {
    case ArtifactType::Model: return "model";
    case ArtifactType::Mesh: return "mesh";
    case ArtifactType::Material: return "material";
    case ArtifactType::Texture: return "texture";
    case ArtifactType::Skeleton: return "skeleton";
    case ArtifactType::Skin: return "skin";
    case ArtifactType::AnimationClip: return "animation";
    case ArtifactType::MorphTarget: return "morph-target";
    case ArtifactType::Prefab: return "prefab";
    case ArtifactType::Scene: return "scene";
    case ArtifactType::Shader: return "shader";
    case ArtifactType::Audio: return "audio";
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        break;
    }
    return "asset";
}

std::string ExpectedPrefabResolvedAssetType(const NLS::Core::Assets::ArtifactType type)
{
    using NLS::Core::Assets::ArtifactType;
    switch (type)
    {
    case ArtifactType::Mesh: return "Mesh";
    case ArtifactType::Material: return "Material";
    case ArtifactType::Texture: return "Texture";
    case ArtifactType::Skeleton: return "Skeleton";
    case ArtifactType::Skin: return "Skin";
    case ArtifactType::AnimationClip: return "AnimationClip";
    case ArtifactType::MorphTarget: return "MorphTarget";
    case ArtifactType::Model: return "Model";
    case ArtifactType::Shader: return "Shader";
    case ArtifactType::Scene: return "Scene";
    case ArtifactType::Audio: return "Audio";
    case ArtifactType::Prefab:
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        return {};
    }
    return {};
}

bool IsContentStorageArtifact(const NLS::Core::Assets::ImportedArtifact& artifact)
{
    return NLS::Core::Assets::IsContentStorageArtifactPath(artifact.artifactPath);
}

std::filesystem::path PortableArtifactPathForRoot(
    const std::filesystem::path& artifactRoot,
    const std::string& artifactPath)
{
    if (artifactRoot.empty() || artifactPath.empty())
        return {};

    const auto path = NLS::Core::Assets::NormalizeAssetPath(artifactPath);
    if (path.empty())
        return {};

    if (!NLS::Core::Assets::IsArtifactStorageFileName(path.filename().generic_string()))
        return {};

    const auto normalizedRoot = NLS::Core::Assets::NormalizeAssetPath(artifactRoot);
    if (normalizedRoot.empty())
        return {};

    if (path.is_absolute())
    {
        std::error_code error;
        auto relative = std::filesystem::relative(path, normalizedRoot, error);
        if (error || relative.empty() || relative.is_absolute())
            return {};
        if (!NLS::Core::Assets::IsContentStorageArtifactPath(
                (std::filesystem::path("Library") / "Artifacts" / relative).generic_string()))
            return {};
        return (std::filesystem::path("Library") /
            "Artifacts" /
            relative).lexically_normal();
    }

    if (NLS::Core::Assets::IsContentStorageArtifactPath(path.generic_string()))
        return path.lexically_normal();

    if (path.has_parent_path())
        return {};

    return (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(path.filename().generic_string())).lexically_normal();
}

NLS::Core::Assets::ArtifactManifest NormalizeArtifactManifestPathsForRoot(
    NLS::Core::Assets::ArtifactManifest manifest,
    const std::filesystem::path& artifactRoot)
{
    for (auto& artifact : manifest.subAssets)
    {
        const auto portable = PortableArtifactPathForRoot(artifactRoot, artifact.artifactPath);
        if (!portable.empty())
            artifact.artifactPath = portable.generic_string();
    }
    return manifest;
}

NLS::Core::Assets::ArtifactManifest FilterContentStorageArtifacts(
    NLS::Core::Assets::ArtifactManifest manifest)
{
    manifest.subAssets.erase(
        std::remove_if(
            manifest.subAssets.begin(),
            manifest.subAssets.end(),
            [](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return !IsContentStorageArtifact(artifact);
            }),
        manifest.subAssets.end());
    return manifest;
}

}

AssetDatabaseFacade::AssetDatabaseFacade(std::vector<std::filesystem::path> roots)
    : AssetDatabaseFacade(std::move(roots), AssetDatabaseAccessMode::Editor)
{
}

AssetDatabaseFacade::AssetDatabaseFacade(std::vector<EditorAssetRoot> roots)
    : AssetDatabaseFacade(std::move(roots), AssetDatabaseAccessMode::Editor)
{
}

AssetDatabaseFacade::AssetDatabaseFacade(
    std::vector<std::filesystem::path> roots,
    AssetDatabaseAccessMode mode)
    : m_roots(MakeEditorAssetRoots(roots))
    , m_mode(mode)
{
}

AssetDatabaseFacade::AssetDatabaseFacade(
    std::vector<EditorAssetRoot> roots,
    AssetDatabaseAccessMode mode)
    : m_mode(mode)
{
    m_roots.reserve(roots.size());
    for (auto& root : roots)
    {
        if (root.path.empty())
            continue;

        root.path = NLS::Core::Assets::NormalizeAssetPath(root.path);
        if (root.path.empty() || root.path == root.path.root_path())
            continue;

        root.mountPath = std::filesystem::path(NormalizeEditorAssetPath(root.mountPath));
        if (!root.libraryPath.empty())
            root.libraryPath = NLS::Core::Assets::NormalizeAssetPath(root.libraryPath);
        m_roots.push_back(std::move(root));
    }
}

std::shared_ptr<const AssetDatabaseFacade> AssetDatabaseFacade::CreateReadOnlySnapshot(const AssetDatabaseFacade& other)
{
    std::scoped_lock lock(other.m_manifestMutex, other.m_artifactDatabaseCacheMutex);
    auto snapshot = std::shared_ptr<AssetDatabaseFacade>(new AssetDatabaseFacade(other.m_roots, other.m_mode));
    snapshot->m_sourceDatabase = other.m_sourceDatabase;
    snapshot->m_diagnostics = other.m_diagnostics;
    snapshot->m_idByEditorPath = other.m_idByEditorPath;
    snapshot->m_editorPathById = other.m_editorPathById;
    snapshot->m_manifestsBySource = other.m_manifestsBySource;
    snapshot->m_knownCurrentArtifactManifestAssetPaths = other.m_knownCurrentArtifactManifestAssetPaths;
    snapshot->m_objectReferencePickerAssetSnapshots = other.m_objectReferencePickerAssetSnapshots;
    return snapshot;
}

bool AssetDatabaseFacade::EnsureStandardPbrShaderLabSourceAvailable()
{
    const auto standardPbrAssetPath = std::filesystem::path(kStandardPbrShaderAssetPath);
    const auto shaderLibraryAssetPath = std::filesystem::path(kStandardPbrShaderLibraryPath);
    const auto bundledShaderRoot =
        std::filesystem::path("App") /
        "Assets" /
        "Engine" /
        "Shaders";

    for (const auto& root : m_roots)
    {
        if (root.readOnly)
            continue;

        const bool mountedAssetsRoot = root.mountPath == "Assets" || root.path.filename() == "Assets";
        const auto projectRoot = !root.libraryPath.empty()
            ? root.libraryPath.parent_path()
            : (mountedAssetsRoot ? root.path.parent_path() : root.path);
        const auto assetsRoot = mountedAssetsRoot ? root.path : projectRoot / "Assets";
        const auto destination = assetsRoot / standardPbrAssetPath.lexically_relative("Assets");
        const auto source = FindBundledShaderAssetSource(
            bundledShaderRoot / "ShaderLab" / "StandardPBR.shader");
        if (!std::filesystem::is_regular_file(source))
            return false;

        std::error_code error;
        if (!FilesHaveSameContents(source, destination))
        {
            std::filesystem::create_directories(destination.parent_path(), error);
            if (error)
                return false;
            std::filesystem::copy_file(
                source,
                destination,
                std::filesystem::copy_options::overwrite_existing,
                error);
            if (error)
                return false;
        }

        const auto libraryDestination = assetsRoot / shaderLibraryAssetPath.lexically_relative("Assets");
        const auto librarySource = FindBundledShaderAssetSource(
            bundledShaderRoot / "NullusShaderLibrary");
        if (!std::filesystem::is_directory(librarySource))
            return false;
        if (!DirectoriesHaveSameContents(librarySource, libraryDestination))
        {
            if (!CopyDirectoryRecursive(librarySource, libraryDestination))
                return false;
        }
        return true;
    }
    return false;
}

bool AssetDatabaseFacade::Refresh()
{
    m_diagnostics.clear();
    {
        std::lock_guard manifestLock(m_manifestMutex);
        m_knownCurrentArtifactManifestAssetPaths.clear();
        m_objectReferencePickerAssetSnapshots.clear();
    }
    if (!FlushArtifactDatabaseCache())
        return false;
    {
        std::lock_guard cacheLock(m_artifactDatabaseCacheMutex);
        m_artifactDatabasesByPath.clear();
    }
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.Refresh");

    if (m_roots.empty())
    {
        AddDiagnostic(
            NLS::Core::Assets::AssetDiagnosticSeverity::Error,
            "assetdatabase-root-missing",
            {},
            "Asset database facade needs at least one source root.");
        return false;
    }

    return RefreshSourceDatabase();
}

bool AssetDatabaseFacade::ImportAsset(const std::string& assetPath)
{
    return ImportAsset(assetPath, nullptr);
}

bool AssetDatabaseFacade::ImportAsset(const std::string& assetPath, ImportProgressTracker& progressTracker)
{
    return ImportAsset(assetPath, &progressTracker);
}

bool AssetDatabaseFacade::ImportAsset(
    const std::string& assetPath,
    ImportProgressTracker& progressTracker,
    const size_t batchTotalAssets)
{
    return ImportAsset(assetPath, &progressTracker, batchTotalAssets);
}

bool AssetDatabaseFacade::ReimportAsset(const std::string& assetPath)
{
    return ReimportAsset(assetPath, nullptr);
}

bool AssetDatabaseFacade::ReimportAsset(const std::string& assetPath, ImportProgressTracker& progressTracker)
{
    return ReimportAsset(assetPath, &progressTracker);
}

bool AssetDatabaseFacade::ReimportAsset(
    const std::string& assetPath,
    ImportProgressTracker& progressTracker,
    const size_t batchTotalAssets)
{
    return ReimportAsset(assetPath, &progressTracker, {}, batchTotalAssets);
}

bool AssetDatabaseFacade::ReimportAsset(
    const std::string& assetPath,
    ImportProgressTracker& progressTracker,
    const ImportJobId existingJob)
{
    return ReimportAsset(assetPath, &progressTracker, existingJob);
}

bool AssetDatabaseFacade::ReimportAssetFromCurrentDatabase(
    const std::string& assetPath,
    ImportProgressTracker& progressTracker,
    const size_t batchTotalAssets)
{
    return ReimportAsset(assetPath, &progressTracker, {}, batchTotalAssets, false);
}

bool AssetDatabaseFacade::ImportAssetFromCurrentDatabase(
    const std::string& assetPath,
    ImportProgressTracker& progressTracker,
    const size_t batchTotalAssets)
{
    return ImportAsset(assetPath, &progressTracker, batchTotalAssets, false);
}

bool AssetDatabaseFacade::ImportAsset(
    const std::string& assetPath,
    ImportProgressTracker* progressTracker,
    const size_t batchTotalAssets,
    const bool refreshDatabase)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.ImportAsset");

    if (m_assetEditing)
    {
        const auto normalized = NormalizeEditorAssetPath(assetPath);
        if (std::find(m_queuedImports.begin(), m_queuedImports.end(), normalized) == m_queuedImports.end())
            m_queuedImports.push_back(normalized);
        return true;
    }

    ImportJobId job;
    if (progressTracker)
    {
        job = progressTracker->BeginJob({}, NormalizeEditorAssetPath(assetPath), "editor", batchTotalAssets);
        progressTracker->ReportProgress(
            job,
            ImportPhase::Queued,
            0.01,
            "Preparing import");
    }

    const auto ok = RefreshSingle(assetPath, progressTracker, job, refreshDatabase);
    if (progressTracker && job.IsValid())
    {
        const auto current = progressTracker->GetCurrentEvent(job);
        if (current.has_value() && current->terminalStatus == ImportJobTerminalStatus::None)
            progressTracker->FinishJob(
                job,
                ok ? ImportJobTerminalStatus::Succeeded : ImportJobTerminalStatus::Failed,
                m_diagnostics);
    }
    if (ok)
        ++m_completedImports;
    return ok;
}

bool AssetDatabaseFacade::ImportAssetImmediateInternal(
    const std::string& assetPath,
    const bool refreshDatabase)
{
    const auto completedBefore = m_completedImports;
    const bool wasAssetEditing = m_assetEditing;
    m_assetEditing = false;
    const auto ok = ImportAsset(assetPath, nullptr, 1u, refreshDatabase);
    m_assetEditing = wasAssetEditing;
    m_completedImports = completedBefore;
    return ok;
}

bool AssetDatabaseFacade::ReimportAsset(
    const std::string& assetPath,
    ImportProgressTracker* progressTracker,
    const ImportJobId existingJob,
    const size_t batchTotalAssets,
    const bool refreshDatabase)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.ReimportAsset");

    ImportJobId job = existingJob;
    const bool ownsProgressJob = !job.IsValid();
    if (progressTracker)
    {
        if (!job.IsValid())
            job = progressTracker->BeginJob({}, NormalizeEditorAssetPath(assetPath), "editor", batchTotalAssets);
        if (ownsProgressJob)
        {
            progressTracker->ReportProgress(
                job,
                ImportPhase::Queued,
                0.01,
                "Preparing reimport");
        }
    }

    if (progressTracker && job.IsValid() && refreshDatabase)
    {
        const double refreshProgress = ownsProgressJob ? 0.02 : 0.045;
        progressTracker->ReportProgress(job, ImportPhase::Queued, refreshProgress, "Refreshing asset database");
    }
    if (refreshDatabase && !Refresh())
    {
        if (progressTracker && job.IsValid())
            progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
        return false;
    }

    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
    {
        if (progressTracker && job.IsValid())
            progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
        return false;
    }

    if (progressTracker && job.IsValid())
    {
        const double stagingProgress = ownsProgressJob ? 0.04 : 0.048;
        progressTracker->ReportProgress(job, ImportPhase::Queued, stagingProgress, "Preparing artifact staging");
    }
    const auto stagingRoot = GetArtifactStagingRootForAssetPath(record->absolutePath);
    std::error_code error;
    if (!stagingRoot.empty())
        std::filesystem::remove_all(stagingRoot, error);

    const auto ok = RefreshSingle(assetPath, progressTracker, job, false);
    if (progressTracker && job.IsValid())
    {
        const auto current = progressTracker->GetCurrentEvent(job);
        if (current.has_value() && current->terminalStatus == ImportJobTerminalStatus::None)
            progressTracker->FinishJob(
                job,
                ok ? ImportJobTerminalStatus::Succeeded : ImportJobTerminalStatus::Failed,
                m_diagnostics);
    }
    if (ok)
        ++m_completedImports;
    return ok;
}

void AssetDatabaseFacade::StartAssetEditing()
{
    if (!IsEditorMode())
    {
        RejectRuntimeEditorApi("AssetDatabase.StartAssetEditing");
        return;
    }

    m_assetEditing = true;
    m_knownCurrentArtifactManifestSnapshotDirty = false;
}

bool AssetDatabaseFacade::StopAssetEditing()
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.StopAssetEditing");

    auto queuedImports = std::move(m_queuedImports);
    m_queuedImports.clear();

    bool ok = true;
    if (!queuedImports.empty())
        ok = RefreshSourceDatabase() && ok;
    for (const auto& assetPath : queuedImports)
    {
        ok = RefreshSingle(assetPath, nullptr, {}, false) && ok;
        if (ok)
            ++m_completedImports;
    }
    m_assetEditing = false;
    ok = FlushArtifactDatabaseCache() && ok;
    if (m_knownCurrentArtifactManifestSnapshotDirty)
        RefreshKnownCurrentArtifactManifestSnapshot();
    m_knownCurrentArtifactManifestSnapshotDirty = false;
    return ok;
}

std::string AssetDatabaseFacade::AssetPathToGUID(const std::string& assetPath) const
{
    if (!IsEditorMode())
        return {};

    const auto found = m_idByEditorPath.find(NormalizeEditorAssetPath(assetPath));
    if (found == m_idByEditorPath.end())
        return {};
    return found->second.ToString();
}

std::string AssetDatabaseFacade::GUIDToAssetPath(const std::string& guid) const
{
    if (!IsEditorMode())
        return {};

    const auto id = ParseAssetId(guid);
    if (!id.IsValid())
        return {};

    const auto found = m_editorPathById.find(id);
    if (found == m_editorPathById.end())
        return {};
    return found->second;
}

std::optional<AssetDatabaseRecord> AssetDatabaseFacade::LoadMainAssetAtPath(const std::string& assetPath) const
{
    if (!IsEditorMode())
        return std::nullopt;

    std::lock_guard manifestLock(m_manifestMutex);
    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return std::nullopt;

    AssetDatabaseRecord result;
    result.assetId = record->id;
    result.assetPath = ToEditorAssetPath(record->absolutePath);
    result.mainAsset = true;

    const auto manifest = m_manifestsBySource.find(record->id);
    if (manifest != m_manifestsBySource.end())
    {
        if (const auto* artifact = manifest->second.FindPrimaryArtifact())
        {
            if (!IsContentStorageArtifact(*artifact))
            {
                result.artifactType = NLS::Core::Assets::ArtifactType::Unknown;
                return result;
            }
            result.subAssetKey = artifact->subAssetKey;
            result.artifactPath = ResolveArtifactPathForRecord(*record, artifact->artifactPath).string();
            if (result.artifactPath.empty())
                result.artifactPath = artifact->artifactPath;
            result.artifactType = artifact->artifactType;
            return result;
        }
    }

    result.artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    return result;
}

std::vector<AssetDatabaseRecord> AssetDatabaseFacade::LoadAllAssetsAtPath(const std::string& assetPath) const
{
    std::vector<AssetDatabaseRecord> results;
    if (!IsEditorMode())
        return results;

    std::lock_guard manifestLock(m_manifestMutex);
    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return results;

    const auto path = ToEditorAssetPath(record->absolutePath);
    const auto manifest = m_manifestsBySource.find(record->id);
    if (manifest == m_manifestsBySource.end())
    {
        results.push_back({record->id, path, {}, {}, NLS::Core::Assets::ArtifactType::Unknown, true});
        return results;
    }

    results.reserve(manifest->second.subAssets.size());
    for (const auto& artifact : manifest->second.subAssets)
    {
        if (!IsContentStorageArtifact(artifact))
            continue;

        results.push_back({
            record->id,
            path,
            artifact.subAssetKey,
            ResolveArtifactPathForRecord(*record, artifact.artifactPath).string(),
            artifact.artifactType,
            artifact.subAssetKey == manifest->second.primarySubAssetKey
        });
        if (results.back().artifactPath.empty())
            results.back().artifactPath = artifact.artifactPath;
    }
    return results;
}

std::optional<AssetDatabaseRecord> AssetDatabaseFacade::LoadSubAssetAtPath(
    const std::string& assetPath,
    const std::string& subAssetKey) const
{
    if (!IsEditorMode())
        return std::nullopt;

    const auto allAssets = LoadAllAssetsAtPath(assetPath);
    const auto found = std::find_if(
        allAssets.begin(),
        allAssets.end(),
        [&subAssetKey](const AssetDatabaseRecord& record)
        {
            return record.subAssetKey == subAssetKey;
        });
    if (found == allAssets.end())
        return std::nullopt;
    return *found;
}

std::filesystem::path AssetDatabaseFacade::ResolveArtifactPathAtPath(
    const std::string& assetPath,
    const std::string& subAssetKey) const
{
    if (!IsEditorMode())
        return {};

    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return {};

    NLS::Core::Assets::ArtifactManifest manifestCopy;
    {
        std::lock_guard manifestLock(m_manifestMutex);
        const auto foundManifest = m_manifestsBySource.find(record->id);
        if (foundManifest == m_manifestsBySource.end())
            return {};
        manifestCopy = foundManifest->second;
    }

    const auto* artifact = subAssetKey.empty()
        ? manifestCopy.FindPrimaryArtifact()
        : manifestCopy.FindSubAsset(subAssetKey);
    if (artifact == nullptr)
        return {};

    return ResolveArtifactPathForRecord(*record, artifact->artifactPath);
}

std::optional<NLS::Engine::Assets::PrefabArtifact> AssetDatabaseFacade::LoadPrefabArtifactAtPath(
    const std::string& assetPath,
    const std::string& subAssetKey) const
{
    if (!IsEditorMode())
        return std::nullopt;

    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return std::nullopt;

    NLS::Core::Assets::ArtifactManifest manifestCopy;
    NLS::Core::Assets::ImportedArtifact prefabArtifactCopy;
    {
        std::lock_guard manifestLock(m_manifestMutex);
        const auto foundManifest = m_manifestsBySource.find(record->id);
        if (foundManifest == m_manifestsBySource.end())
            return std::nullopt;

        const auto* prefabArtifact = foundManifest->second.FindSubAsset(subAssetKey);
        if (!prefabArtifact || prefabArtifact->artifactType != NLS::Core::Assets::ArtifactType::Prefab)
            return std::nullopt;

        manifestCopy = foundManifest->second;
        prefabArtifactCopy = *prefabArtifact;
    }

    const auto artifactPath = ResolveArtifactPathForRecord(*record, prefabArtifactCopy.artifactPath);
    if (artifactPath.empty())
        return std::nullopt;

    std::ifstream input(artifactPath, std::ios::binary);
    if (!input)
        return std::nullopt;

    const std::vector<uint8_t> bytes {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
        bytes,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u);
    if (!container.has_value())
        return std::nullopt;

    std::vector<NLS::Engine::Assets::PrefabResolvedAsset> resolvedAssets;
    for (const auto& artifact : manifestCopy.subAssets)
    {
        if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab)
            continue;

        auto expectedType = ExpectedPrefabResolvedAssetType(artifact.artifactType);

        if (!expectedType.empty())
        {
            resolvedAssets.push_back({
                artifact.sourceAssetId,
                std::move(expectedType),
                artifact.subAssetKey,
                artifact.artifactPath
            });
        }
    }

    const std::string payload(container->payload.begin(), container->payload.end());
    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        payload,
        record->id,
        std::move(resolvedAssets));
    if (importResult.diagnostics.HasErrors())
        return std::nullopt;

    auto prefab = std::move(importResult.artifact);
    prefab.generatedModelPrefab = record->assetType == NLS::Core::Assets::AssetType::ModelScene;

    return prefab;
}

std::optional<NLS::Engine::Assets::PrefabArtifact> AssetDatabaseFacade::LoadPrefabArtifactByAssetId(
    const NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey) const
{
    if (!IsEditorMode() || !assetId.IsValid() || subAssetKey.empty())
        return std::nullopt;

    NLS::Core::Assets::ArtifactManifest manifestCopy;
    NLS::Core::Assets::ImportedArtifact prefabArtifactCopy;
    {
        std::lock_guard manifestLock(m_manifestMutex);
        const auto foundManifest = m_manifestsBySource.find(assetId);
        if (foundManifest != m_manifestsBySource.end())
            manifestCopy = foundManifest->second;
    }

    if (!manifestCopy.sourceAssetId.IsValid())
    {
        for (const auto& root : m_roots)
        {
            const auto databasePath = GetEditorAssetRootLibraryPath(root) / "ArtifactDB";
            NLS::Core::Assets::ArtifactDatabase database;
            if (!database.Load(databasePath))
                continue;

            auto persistedManifest = database.BuildManifestForSource(assetId);
            if (persistedManifest.has_value() && persistedManifest->sourceAssetId == assetId)
            {
                manifestCopy = FilterContentStorageArtifacts(std::move(*persistedManifest));
                break;
            }
        }
    }

    if (!manifestCopy.sourceAssetId.IsValid())
        return std::nullopt;

    const auto* prefabArtifact = manifestCopy.FindSubAsset(subAssetKey);
    if (!prefabArtifact || prefabArtifact->artifactType != NLS::Core::Assets::ArtifactType::Prefab)
        return std::nullopt;
    prefabArtifactCopy = *prefabArtifact;

    std::filesystem::path artifactPath;
    const auto rawPath = std::filesystem::path(prefabArtifactCopy.artifactPath).lexically_normal();
    for (const auto& root : m_roots)
    {
        artifactPath = ResolvePhysicalArtifactFileFromLibraryPath(root, rawPath);
        if (!artifactPath.empty())
            break;
    }

    if (artifactPath.empty())
        return std::nullopt;

    std::ifstream input(artifactPath, std::ios::binary);
    if (!input)
        return std::nullopt;

    const std::vector<uint8_t> bytes {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
        bytes,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u);
    if (!container.has_value())
        return std::nullopt;

    std::vector<NLS::Engine::Assets::PrefabResolvedAsset> resolvedAssets;
    for (const auto& artifact : manifestCopy.subAssets)
    {
        if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab)
            continue;

        auto expectedType = ExpectedPrefabResolvedAssetType(artifact.artifactType);
        if (!expectedType.empty())
        {
            resolvedAssets.push_back({
                artifact.sourceAssetId,
                std::move(expectedType),
                artifact.subAssetKey,
                artifact.artifactPath
            });
        }
    }

    const std::string payload(container->payload.begin(), container->payload.end());
    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        payload,
        assetId,
        std::move(resolvedAssets));
    if (importResult.diagnostics.HasErrors())
        return std::nullopt;

    auto prefab = std::move(importResult.artifact);
    prefab.generatedModelPrefab = manifestCopy.importerId == "scene-model";
    return prefab;
}

bool AssetDatabaseFacade::MoveAsset(
    const std::string& sourceAssetPath,
    const std::string& destinationAssetPath)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.MoveAsset");

    const auto source = ResolveAssetPath(sourceAssetPath);
    const auto destination = ResolveAssetPath(destinationAssetPath);
    if (source.empty() || destination.empty() || !std::filesystem::exists(source) ||
        std::filesystem::exists(destination) || !IsMutableAssetRecord(sourceAssetPath) ||
        !IsWritableAssetPath(destination))
    {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
        return false;

    std::filesystem::rename(source, destination, error);
    if (error)
        return false;

    const auto sourceMeta = NLS::Core::Assets::GetAssetMetaPath(source);
    if (std::filesystem::exists(sourceMeta))
    {
        std::filesystem::rename(
            sourceMeta,
            NLS::Core::Assets::GetAssetMetaPath(destination),
            error);
        if (error)
            return false;
    }

    return Refresh();
}

bool AssetDatabaseFacade::RenameAsset(const std::string& assetPath, const std::string& newName)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.RenameAsset");

    if (!IsSingleEditorAssetName(newName))
        return false;

    const auto source = std::filesystem::path(assetPath);
    const auto destination = std::filesystem::path(ParentEditorAssetPath(assetPath)) / std::filesystem::path(newName);
    return MoveAsset(source.generic_string(), destination.generic_string());
}

bool AssetDatabaseFacade::CopyAsset(
    const std::string& sourceAssetPath,
    const std::string& destinationAssetPath)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.CopyAsset");

    const auto source = ResolveAssetPath(sourceAssetPath);
    const auto destination = ResolveAssetPath(destinationAssetPath);
    if (source.empty() || destination.empty() || !std::filesystem::exists(source) ||
        std::filesystem::exists(destination) || !IsMutableAssetRecord(sourceAssetPath) ||
        !IsWritableAssetPath(destination))
    {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
        return false;

    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
    if (error)
        return false;

    auto meta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(source))
        .value_or(NLS::Core::Assets::AssetMeta::CreateForAsset(destination));
    meta.id = NLS::Core::Assets::AssetId::New();
    meta.assetType = NLS::Core::Assets::InferAssetType(destination);
    meta.importerId = NLS::Core::Assets::InferImporterId(meta.assetType);
    if (!meta.Save(NLS::Core::Assets::GetAssetMetaPath(destination)))
        return false;

    return Refresh();
}

bool AssetDatabaseFacade::DeleteAsset(const std::string& assetPath)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.DeleteAsset");

    const auto absolutePath = ResolveAssetPath(assetPath);
    if (absolutePath.empty())
        return false;
    if (!IsMutableAssetRecord(assetPath))
        return false;
    if (!std::filesystem::exists(absolutePath))
        return false;

    std::error_code error;
    std::filesystem::remove_all(absolutePath, error);
    if (error)
        return false;

    const auto metaPath = NLS::Core::Assets::GetAssetMetaPath(absolutePath);
    if (std::filesystem::exists(metaPath))
    {
        std::filesystem::remove(metaPath, error);
        if (error)
            return false;
    }

    return Refresh();
}

std::string AssetDatabaseFacade::CreateFolder(
    const std::string& parentFolder,
    const std::string& folderName)
{
    if (!IsEditorMode())
    {
        RejectRuntimeEditorApi("AssetDatabase.CreateFolder");
        return {};
    }

    const auto parentPath = ResolveAssetPath(parentFolder);
    if (parentPath.empty())
        return {};
    if (!IsWritableAssetPath(parentPath))
        return {};

    if (!IsSingleEditorAssetName(folderName))
        return {};

    auto folderPath = NLS::Core::Assets::NormalizeAssetPath(parentPath / folderName);
    if (ResolveAssetPath(ToEditorAssetPath(folderPath)).empty())
        return {};

    std::filesystem::create_directories(folderPath);
    return ToEditorAssetPath(folderPath);
}

bool AssetDatabaseFacade::IsValidFolder(const std::string& assetPath) const
{
    if (!IsEditorMode())
        return false;

    const auto absolutePath = ResolveAssetPath(assetPath);
    if (absolutePath.empty())
        return false;
    return std::filesystem::exists(absolutePath) && std::filesystem::is_directory(absolutePath);
}

std::string AssetDatabaseFacade::GenerateUniqueAssetPath(const std::string& desiredAssetPath) const
{
    if (!IsEditorMode())
        return {};

    const auto desired = std::filesystem::path(NormalizeEditorAssetPath(desiredAssetPath));
    const auto desiredAbsolute = ResolveAssetPath(desired.generic_string());
    if (desiredAbsolute.empty())
        return {};
    if (!std::filesystem::exists(desiredAbsolute))
        return desired.generic_string();

    const auto parent = desired.parent_path();
    const auto stem = desired.stem().string();
    const auto extension = desired.extension().string();
    for (size_t index = 1u; index < 100000u; ++index)
    {
        const auto candidate = parent / (stem + " " + std::to_string(index) + extension);
        if (!std::filesystem::exists(ResolveAssetPath(candidate.generic_string())))
            return candidate.generic_string();
    }
    return {};
}

void AssetDatabaseFacade::AddArtifactManifest(NLS::Core::Assets::ArtifactManifest manifest)
{
    if (!IsEditorMode())
        return;

    manifest = FilterContentStorageArtifacts(std::move(manifest));
    const auto assetPath = GUIDToAssetPath(manifest.sourceAssetId.ToString());
    if (!SaveArtifactDatabaseManifest(manifest))
        return;
    {
        std::lock_guard manifestLock(m_manifestMutex);
        m_manifestsBySource[manifest.sourceAssetId] = std::move(manifest);
    }
    if (!assetPath.empty() && !m_assetEditing)
        UpdateKnownCurrentArtifactManifestForAssetPath(assetPath);
    else if (!assetPath.empty())
        m_knownCurrentArtifactManifestSnapshotDirty = true;
}

std::optional<NLS::Core::Assets::ArtifactManifest> AssetDatabaseFacade::GetArtifactManifestForAssetPath(
    const std::string& assetPath) const
{
    if (!IsEditorMode())
        return std::nullopt;

    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return std::nullopt;

    std::lock_guard manifestLock(m_manifestMutex);
    const auto manifest = m_manifestsBySource.find(record->id);
    if (manifest == m_manifestsBySource.end())
        return std::nullopt;

    return manifest->second;
}

bool AssetDatabaseFacade::IsArtifactManifestCurrentForAssetPath(const std::string& assetPath) const
{
    const bool current = IsArtifactManifestCurrentForAssetPathUncached(assetPath);
    if (!current)
    {
        const auto normalized = NormalizeEditorAssetPath(assetPath);
        if (!normalized.empty())
        {
            std::lock_guard manifestLock(m_manifestMutex);
            RemoveKnownCurrentArtifactManifestSnapshotForAssetPathLocked(normalized);
        }
    }
    return current;
}

bool AssetDatabaseFacade::IsArtifactManifestCurrentForAssetPathUncached(const std::string& assetPath) const
{
    if (!IsEditorMode())
        return false;

    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return false;

    NLS::Core::Assets::ArtifactManifest manifestCopy;
    {
        std::lock_guard manifestLock(m_manifestMutex);
        const auto manifest = m_manifestsBySource.find(record->id);
        if (manifest == m_manifestsBySource.end())
            return false;
        manifestCopy = manifest->second;
    }

    auto meta = NLS::Core::Assets::AssetMeta::Load(record->metaPath);
    if (!meta.has_value())
        return false;
    meta->importerVersion = std::max(meta->importerVersion, record->importerVersion);

    if (manifestCopy.importerId != meta->importerId ||
        manifestCopy.importerVersion != meta->importerVersion ||
        manifestCopy.targetPlatform != "editor" ||
        !HasCurrentExternalTextureBuildPipelineDependency(manifestCopy, meta->assetType) ||
        !HasCurrentShaderCompilerToolchainDependency(manifestCopy, meta->assetType))
    {
        return false;
    }

    for (const auto& artifact : manifestCopy.subAssets)
    {
        const auto artifactPath = ResolveArtifactPathForRecord(*record, artifact.artifactPath);
        if (artifactPath.empty() || !std::filesystem::is_regular_file(artifactPath))
            return false;
    }

    const auto normalizedAssetPath = NormalizeEditorAssetPath(ToEditorAssetPath(record->absolutePath));
    const auto normalizedMetaPath = NormalizeEditorAssetPath(
        ToEditorAssetPath(NLS::Core::Assets::GetAssetMetaPath(record->absolutePath)));

    bool checkedAsset = false;
    bool checkedMeta = false;
    for (const auto& dependency : manifestCopy.dependencies)
    {
        if (!IsStampDependencyKind(dependency.kind))
            continue;

        const auto normalizedValue = NormalizeEditorAssetPath(dependency.value);
        const auto checksPrimaryAsset =
            dependency.kind == NLS::Core::Assets::AssetDependencyKind::SourceFileHash &&
            normalizedValue == normalizedAssetPath;
        const auto checksPrimaryMeta =
            dependency.kind == NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping &&
            normalizedValue == normalizedMetaPath;

        const auto dependencyPath = ResolveManifestStampDependencyPath(
            m_roots,
            *record,
            dependency.value,
            checksPrimaryAsset || checksPrimaryMeta);
        if (dependencyPath.empty() || FileStamp(dependencyPath) != dependency.hashOrVersion)
            return false;

        if (checksPrimaryAsset)
        {
            checkedAsset = true;
        }
        else if (checksPrimaryMeta)
        {
            checkedMeta = true;
        }
    }

    const bool checkedImporterVersion = HasDependency(
        manifestCopy,
        NLS::Core::Assets::AssetDependencyKind::ImporterVersion,
        meta->importerId,
        std::to_string(meta->importerVersion));
    const bool checkedBuildTarget = HasDependency(
        manifestCopy,
        NLS::Core::Assets::AssetDependencyKind::BuildTarget,
        "editor",
        "editor");

    return checkedAsset && checkedMeta && checkedImporterVersion && checkedBuildTarget;
}

bool AssetDatabaseFacade::IsArtifactManifestKnownCurrentForAssetPath(const std::string& assetPath) const
{
    std::lock_guard manifestLock(m_manifestMutex);
    return m_knownCurrentArtifactManifestAssetPaths.find(NormalizeEditorAssetPath(assetPath)) !=
        m_knownCurrentArtifactManifestAssetPaths.end();
}

std::vector<std::string> AssetDatabaseFacade::GetKnownCurrentArtifactManifestAssetPaths() const
{
    std::lock_guard manifestLock(m_manifestMutex);
    std::vector<std::string> assetPaths(
        m_knownCurrentArtifactManifestAssetPaths.begin(),
        m_knownCurrentArtifactManifestAssetPaths.end());
    std::sort(assetPaths.begin(), assetPaths.end());
    return assetPaths;
}

ObjectReferencePickerAssetSnapshot AssetDatabaseFacade::BuildObjectReferencePickerAssetSnapshot(
    const std::string& assetPath,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    const auto normalized = NormalizeEditorAssetPath(assetPath);
    ObjectReferencePickerAssetSnapshot snapshot;
    snapshot.sourceAssetPath = normalized;
    snapshot.assetId = manifest.sourceAssetId;
    snapshot.subAssets.reserve(manifest.subAssets.size());
    for (const auto& artifact : manifest.subAssets)
    {
        snapshot.subAssets.push_back({
            artifact.subAssetKey,
            artifact.artifactPath,
            artifact.artifactType,
            ReadSubAssetDisplayNameForSnapshot(artifact)
        });
    }
    return snapshot;
}

void AssetDatabaseFacade::ReplaceKnownCurrentArtifactManifestSnapshotsLocked(
    std::unordered_set<std::string> assetPaths,
    std::vector<ObjectReferencePickerAssetSnapshot> snapshots) const
{
    m_knownCurrentArtifactManifestAssetPaths = std::move(assetPaths);

    snapshots.erase(
        std::remove_if(
            snapshots.begin(),
            snapshots.end(),
            [](const ObjectReferencePickerAssetSnapshot& snapshot)
            {
                return snapshot.sourceAssetPath.empty() || snapshot.subAssets.empty();
            }),
        snapshots.end());
    std::sort(
        snapshots.begin(),
        snapshots.end(),
        [](const ObjectReferencePickerAssetSnapshot& left, const ObjectReferencePickerAssetSnapshot& right)
        {
            return left.sourceAssetPath < right.sourceAssetPath;
        });
    m_objectReferencePickerAssetSnapshots = std::move(snapshots);
}

void AssetDatabaseFacade::RemoveKnownCurrentArtifactManifestSnapshotForAssetPathLocked(const std::string& assetPath) const
{
    const auto normalized = NormalizeEditorAssetPath(assetPath);
    m_knownCurrentArtifactManifestAssetPaths.erase(normalized);
    m_objectReferencePickerAssetSnapshots.erase(
        std::remove_if(
            m_objectReferencePickerAssetSnapshots.begin(),
            m_objectReferencePickerAssetSnapshots.end(),
            [&normalized](const ObjectReferencePickerAssetSnapshot& snapshot)
            {
                return NormalizeEditorAssetPath(snapshot.sourceAssetPath) == normalized;
            }),
        m_objectReferencePickerAssetSnapshots.end());
}

void AssetDatabaseFacade::PublishKnownCurrentArtifactManifestSnapshotForAssetPathLocked(
    const std::string& assetPath,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    const auto normalized = NormalizeEditorAssetPath(assetPath);
    if (normalized.empty())
        return;

    RemoveKnownCurrentArtifactManifestSnapshotForAssetPathLocked(normalized);
    m_knownCurrentArtifactManifestAssetPaths.insert(normalized);

    auto snapshot = BuildObjectReferencePickerAssetSnapshot(normalized, manifest);

    if (!snapshot.subAssets.empty())
        m_objectReferencePickerAssetSnapshots.push_back(std::move(snapshot));
    std::sort(
        m_objectReferencePickerAssetSnapshots.begin(),
        m_objectReferencePickerAssetSnapshots.end(),
        [](const ObjectReferencePickerAssetSnapshot& left, const ObjectReferencePickerAssetSnapshot& right)
        {
            return left.sourceAssetPath < right.sourceAssetPath;
        });
}

void AssetDatabaseFacade::PruneStaleObjectReferencePickerSnapshots() const
{
    std::vector<std::string> candidatePaths;
    {
        std::lock_guard manifestLock(m_manifestMutex);
        candidatePaths.reserve(m_objectReferencePickerAssetSnapshots.size());
        for (const auto& snapshot : m_objectReferencePickerAssetSnapshots)
        {
            const auto normalized = NormalizeEditorAssetPath(snapshot.sourceAssetPath);
            if (!normalized.empty())
                candidatePaths.push_back(normalized);
        }
    }

    std::sort(candidatePaths.begin(), candidatePaths.end());
    candidatePaths.erase(std::unique(candidatePaths.begin(), candidatePaths.end()), candidatePaths.end());
    if (candidatePaths.empty())
        return;

    std::unordered_set<std::string> currentPaths;
    std::unordered_set<std::string> stalePaths;
    std::vector<ObjectReferencePickerAssetSnapshot> rebuiltSnapshots;
    for (const auto& assetPath : candidatePaths)
    {
        if (!IsArtifactManifestCurrentForAssetPath(assetPath))
        {
            stalePaths.insert(assetPath);
            continue;
        }

        const auto* record = FindRecordByEditorAssetPath(assetPath);
        if (!record)
        {
            stalePaths.insert(assetPath);
            continue;
        }

        std::optional<NLS::Core::Assets::ArtifactManifest> manifest;
        {
            std::lock_guard manifestLock(m_manifestMutex);
            const auto found = m_manifestsBySource.find(record->id);
            if (found != m_manifestsBySource.end())
                manifest = found->second;
        }
        if (!manifest.has_value())
        {
            stalePaths.insert(assetPath);
            continue;
        }

        currentPaths.insert(assetPath);
        auto snapshot = BuildObjectReferencePickerAssetSnapshot(assetPath, *manifest);
        if (!snapshot.subAssets.empty())
            rebuiltSnapshots.push_back(std::move(snapshot));
    }

    std::lock_guard manifestLock(m_manifestMutex);
    m_objectReferencePickerAssetSnapshots.erase(
        std::remove_if(
            m_objectReferencePickerAssetSnapshots.begin(),
            m_objectReferencePickerAssetSnapshots.end(),
            [&candidatePaths](const ObjectReferencePickerAssetSnapshot& snapshot)
            {
                const auto normalized = NormalizeEditorAssetPath(snapshot.sourceAssetPath);
                return std::binary_search(candidatePaths.begin(), candidatePaths.end(), normalized);
            }),
        m_objectReferencePickerAssetSnapshots.end());
    for (const auto& stalePath : stalePaths)
        m_knownCurrentArtifactManifestAssetPaths.erase(stalePath);
    for (const auto& currentPath : currentPaths)
        m_knownCurrentArtifactManifestAssetPaths.insert(currentPath);
    for (auto& snapshot : rebuiltSnapshots)
        m_objectReferencePickerAssetSnapshots.push_back(std::move(snapshot));
    std::sort(
        m_objectReferencePickerAssetSnapshots.begin(),
        m_objectReferencePickerAssetSnapshots.end(),
        [](const ObjectReferencePickerAssetSnapshot& left, const ObjectReferencePickerAssetSnapshot& right)
        {
            return left.sourceAssetPath < right.sourceAssetPath;
        });
}

std::vector<ObjectReferencePickerAssetSnapshot> AssetDatabaseFacade::GetObjectReferencePickerAssetSnapshots() const
{
    if (!IsEditorMode())
        return {};

    std::lock_guard manifestLock(m_manifestMutex);
    return m_objectReferencePickerAssetSnapshots;
}

std::vector<ObjectReferencePickerAssetSnapshot> AssetDatabaseFacade::GetFreshObjectReferencePickerAssetSnapshots() const
{
    if (!IsEditorMode())
        return {};

    PruneStaleObjectReferencePickerSnapshots();

    std::lock_guard manifestLock(m_manifestMutex);
    return m_objectReferencePickerAssetSnapshots;
}

std::optional<std::string> AssetDatabaseFacade::TryGetRootRelativeAssetPath(
    const std::string& ownerAssetPath,
    const std::filesystem::path& path) const
{
    if (!IsEditorMode() || path.empty())
        return std::nullopt;

    const auto* ownerRecord = FindRecordByEditorAssetPath(ownerAssetPath);
    if (!ownerRecord)
        return std::nullopt;

    const auto* root = FindEditorAssetRootForAbsolutePath(m_roots, ownerRecord->absolutePath);
    if (!root)
        return std::nullopt;

    const auto absolutePath = path.is_absolute()
        ? NLS::Core::Assets::NormalizeAssetPath(path)
        : ResolveAssetPath(NormalizeEditorAssetPath(path));
    if (absolutePath.empty() ||
        FindEditorAssetRootForAbsolutePath(m_roots, absolutePath) != root ||
        !IsPathInsideEditorAssetRoot(absolutePath, root->path) ||
        !IsPhysicalPathInsideEditorAssetRoot(absolutePath, root->path))
    {
        return std::nullopt;
    }

    return NormalizeEditorAssetPath(absolutePath.lexically_relative(root->path));
}

std::vector<std::string> AssetDatabaseFacade::GetDependencies(
    const std::string& assetPath,
    const bool recursive) const
{
    std::vector<std::string> results;
    if (!IsEditorMode())
        return results;

    std::lock_guard manifestLock(m_manifestMutex);
    std::unordered_set<NLS::Core::Assets::AssetId> visited;
    std::vector<NLS::Core::Assets::AssetId> frontier;

    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return results;
    frontier.push_back(record->id);
    visited.insert(record->id);

    while (!frontier.empty())
    {
        const auto current = frontier.back();
        frontier.pop_back();

        const auto manifest = m_manifestsBySource.find(current);
        if (manifest == m_manifestsBySource.end())
            continue;

        for (const auto& dependency : manifest->second.dependencies)
        {
            if (!IsDependencyKindAssetReference(dependency.kind))
                continue;

            const auto dependencyId = ParseAssetId(dependency.value);
            if (!dependencyId.IsValid())
                continue;

            const auto path = m_editorPathById.find(dependencyId);
            if (path != m_editorPathById.end() &&
                std::find(results.begin(), results.end(), path->second) == results.end())
            {
                results.push_back(path->second);
            }

            if (recursive && visited.insert(dependencyId).second)
                frontier.push_back(dependencyId);
        }
    }

    std::sort(results.begin(), results.end());
    return results;
}

std::vector<std::string> AssetDatabaseFacade::FindAssets(
    const std::string& filter,
    const std::vector<std::string>& searchInFolders) const
{
    const auto parsed = ParseSearchFilter(filter);
    std::vector<std::string> results;
    if (!IsEditorMode())
        return results;

    for (const auto& record : m_sourceDatabase.GetRecords())
    {
        const auto editorAssetPath = ToEditorAssetPath(record.absolutePath);
        if (!IsPathInSearchFolders(editorAssetPath, searchInFolders))
            continue;

        if (!parsed.name.empty() &&
            !ContainsLower(std::filesystem::path(editorAssetPath).stem().string(), parsed.name))
        {
            continue;
        }

        if (!parsed.type.empty() && ToLower(NLS::Core::Assets::ToString(record.assetType)) != parsed.type)
            continue;

        if (!parsed.label.empty())
        {
            const auto labels = GetLabels(editorAssetPath);
            const auto hasLabel = std::any_of(
                labels.begin(),
                labels.end(),
                [&parsed](const std::string& label)
                {
                    return ToLower(label) == parsed.label;
                });
            if (!hasLabel)
                continue;
        }

        results.push_back(editorAssetPath);
    }

    std::sort(results.begin(), results.end());
    return results;
}

bool AssetDatabaseFacade::SetLabels(const std::string& assetPath, std::vector<std::string> labels)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.SetLabels");

    auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return false;

    meta->settings["LABELS"] = JoinList(std::move(labels));
    return SaveMetaForPath(assetPath, *meta);
}

std::vector<std::string> AssetDatabaseFacade::GetLabels(const std::string& assetPath) const
{
    if (!IsEditorMode())
        return {};

    const auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return {};

    const auto found = meta->settings.find("LABELS");
    if (found == meta->settings.end())
        return {};
    return SplitList(found->second);
}

std::vector<std::string> AssetDatabaseFacade::GetAllLabels() const
{
    std::vector<std::string> labels;
    if (!IsEditorMode())
        return labels;

    for (const auto& record : m_sourceDatabase.GetRecords())
    {
        auto recordLabels = GetLabels(ToEditorAssetPath(record.absolutePath));
        labels.insert(labels.end(), recordLabels.begin(), recordLabels.end());
    }

    std::sort(labels.begin(), labels.end());
    labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
    return labels;
}

bool AssetDatabaseFacade::SetAssetPackNameAndVariant(
    const std::string& assetPath,
    std::string packName,
    std::string packVariant)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.SetAssetPackNameAndVariant");

    auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return false;

    meta->settings["ASSET_PACK_NAME"] = std::move(packName);
    meta->settings["ASSET_PACK_VARIANT"] = std::move(packVariant);
    return SaveMetaForPath(assetPath, *meta);
}

std::optional<AssetPackMetadata> AssetDatabaseFacade::GetAssetPackNameAndVariant(
    const std::string& assetPath) const
{
    if (!IsEditorMode())
        return std::nullopt;

    const auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return std::nullopt;

    AssetPackMetadata info;
    if (const auto name = meta->settings.find("ASSET_PACK_NAME"); name != meta->settings.end())
        info.name = name->second;
    if (const auto variant = meta->settings.find("ASSET_PACK_VARIANT"); variant != meta->settings.end())
        info.variant = variant->second;

    if (info.name.empty() && info.variant.empty())
        return std::nullopt;
    return info;
}

std::optional<std::string> AssetDatabaseFacade::TryGetEditorAssetPath(
    const std::filesystem::path& path) const
{
    if (!IsEditorMode() || path.empty())
        return std::nullopt;

    if (path.is_absolute())
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(path);
        const auto* root = FindEditorAssetRootForAbsolutePath(m_roots, normalized);
        if (!root || !IsPhysicalPathInsideEditorAssetRoot(normalized, root->path))
            return std::nullopt;

        const auto editorPath = ToEditorAssetPath(normalized);
        if (editorPath.empty())
            return std::nullopt;
        return NormalizeEditorAssetPath(editorPath);
    }

    auto normalized = NormalizeEditorAssetPath(path);
    if (normalized.empty())
        return std::nullopt;

    if (!ResolveAssetPath(normalized).empty())
        return normalized;

    return std::nullopt;
}

std::vector<NLS::Engine::Assets::AssetPackBuildInput> AssetDatabaseFacade::GetAssetPackBuildInputs() const
{
    std::vector<NLS::Engine::Assets::AssetPackBuildInput> inputs;
    if (!IsEditorMode())
        return inputs;

    std::lock_guard manifestLock(m_manifestMutex);
    for (const auto& record : m_sourceDatabase.GetRecords())
    {
        const auto bundle = GetAssetPackNameAndVariant(ToEditorAssetPath(record.absolutePath));
        if (!bundle.has_value() || bundle->name.empty())
            continue;

        const auto manifest = m_manifestsBySource.find(record.id);
        if (manifest == m_manifestsBySource.end() || manifest->second.primarySubAssetKey.empty())
            continue;

        inputs.push_back({
            bundle->name,
            bundle->variant,
            {record.id, manifest->second.primarySubAssetKey}
        });
    }

    std::sort(
        inputs.begin(),
        inputs.end(),
        [](const auto& lhs, const auto& rhs)
        {
            if (lhs.packName != rhs.packName)
                return lhs.packName < rhs.packName;
            if (lhs.packVariant != rhs.packVariant)
                return lhs.packVariant < rhs.packVariant;
            return lhs.root.assetId.ToString() < rhs.root.assetId.ToString();
        });
    return inputs;
}

bool AssetDatabaseFacade::CreateAsset(
    const AssetObjectRecord& asset,
    const std::string& assetPath)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.CreateAsset");

    const auto absolutePath = ResolveAssetPath(assetPath);
    if (absolutePath.empty() || std::filesystem::exists(absolutePath) || !IsWritableAssetPath(absolutePath))
        return false;

    const auto subAssetKey = MakeSubAssetKey(asset);
    if (!WriteNativeAssetPayload(absolutePath, asset, subAssetKey))
        return false;

    auto meta = NLS::Core::Assets::AssetMeta::CreateForAsset(absolutePath);
    if (meta.assetType == NLS::Core::Assets::AssetType::Unknown)
        meta.assetType = NLS::Core::Assets::AssetType::Material;
    meta.importerId = NLS::Core::Assets::InferImporterId(meta.assetType);
    if (!meta.Save(NLS::Core::Assets::GetAssetMetaPath(absolutePath)))
        return false;

    if (!Refresh())
        return false;

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = meta.id;
    manifest.importerId = meta.importerId;
    manifest.importerVersion = meta.importerVersion;
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = subAssetKey;
    NLS::Core::Assets::ArtifactWriteRequest writeRequest;
    writeRequest.sourceAssetId = meta.id;
    writeRequest.importerId = meta.importerId;
    writeRequest.importerVersion = meta.importerVersion;
    writeRequest.targetPlatform = "editor";
    writeRequest.primarySubAssetKey = subAssetKey;
    writeRequest.artifacts.push_back({
        subAssetKey,
        asset.artifactType,
        asset.loaderId.empty() ? ToArtifactTypeKey(asset.artifactType) : asset.loaderId,
        asset.name,
        ToArtifactTypeKey(asset.artifactType),
        std::vector<uint8_t>(asset.serializedPayload.begin(), asset.serializedPayload.end())
    });
    AddNativeAssetManifestDependencies(
        writeRequest.dependencies,
        ToEditorAssetPath(absolutePath),
        ToEditorAssetPath(NLS::Core::Assets::GetAssetMetaPath(absolutePath)),
        absolutePath,
        NLS::Core::Assets::GetAssetMetaPath(absolutePath),
        meta);

    NLS::Core::Assets::ArtifactWriter writer(
        GetArtifactStagingRootForAssetPath(absolutePath),
        GetArtifactRootForAssetPath(absolutePath));
    const auto writeResult = writer.WriteAndCommit(writeRequest, nullptr);
    if (!writeResult.committed || HasErrors(writeResult.diagnostics))
        return false;

    if (!SaveArtifactManifestForAssetPath(absolutePath, writeResult.manifest))
        return false;
    AddArtifactManifest(writeResult.manifest);
    return true;
}

bool AssetDatabaseFacade::CreateTextAsset(
    const std::string& contents,
    const std::string& assetPath,
    NLS::Core::Assets::AssetId assetId)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.CreateTextAsset");

    const auto absolutePath = ResolveAssetPath(assetPath);
    if (absolutePath.empty() || std::filesystem::exists(absolutePath) || !IsWritableAssetPath(absolutePath))
        return false;

    std::error_code error;
    std::filesystem::create_directories(absolutePath.parent_path(), error);
    if (error)
        return false;

    std::ofstream output(absolutePath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output << contents;
    output.close();
    if (!output)
        return false;

    auto meta = NLS::Core::Assets::AssetMeta::CreateForAsset(absolutePath);
    if (assetId.IsValid())
        meta.id = assetId;
    if (!meta.Save(NLS::Core::Assets::GetAssetMetaPath(absolutePath)))
        return false;

    return Refresh();
}

bool AssetDatabaseFacade::AddObjectToAsset(
    const AssetObjectRecord& asset,
    const std::string& assetPath)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.AddObjectToAsset");

    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return false;

    std::lock_guard manifestLock(m_manifestMutex);
    auto manifest = m_manifestsBySource[record->id];
    if (!manifest.sourceAssetId.IsValid())
    {
        manifest.sourceAssetId = record->id;
        manifest.importerId = record->importerId;
        manifest.importerVersion = record->importerVersion;
        manifest.targetPlatform = "editor";
    }

    const auto subAssetKey = MakeSubAssetKey(asset);
    if (manifest.FindSubAsset(subAssetKey))
        return false;

    if (manifest.primarySubAssetKey.empty())
        manifest.primarySubAssetKey = subAssetKey;

    NLS::Core::Assets::ArtifactWriteRequest writeRequest;
    writeRequest.sourceAssetId = record->id;
    writeRequest.importerId = manifest.importerId.empty() ? record->importerId : manifest.importerId;
    writeRequest.importerVersion = record->importerVersion;
    writeRequest.targetPlatform = "editor";
    writeRequest.primarySubAssetKey = manifest.primarySubAssetKey;
    writeRequest.dependencies = manifest.dependencies;
    auto meta = NLS::Core::Assets::AssetMeta::Load(record->metaPath);
    if (!meta.has_value())
        return false;
    meta->importerVersion = std::max(meta->importerVersion, record->importerVersion);
    AddNativeAssetManifestDependencies(
        writeRequest.dependencies,
        ToEditorAssetPath(record->absolutePath),
        ToEditorAssetPath(record->metaPath),
        record->absolutePath,
        record->metaPath,
        *meta);
    for (const auto& existing : manifest.subAssets)
    {
        const auto resolvedPath = ResolveArtifactPathForRecord(*record, existing.artifactPath);
        std::vector<uint8_t> payload;
        if (!resolvedPath.empty())
            payload = ReadBinaryFile(resolvedPath);
        writeRequest.artifacts.push_back({
            existing.subAssetKey,
            existing.artifactType,
            existing.loaderId,
            existing.displayName,
            ToArtifactTypeKey(existing.artifactType),
            std::move(payload)
        });
    }
    writeRequest.artifacts.push_back({
        subAssetKey,
        asset.artifactType,
        asset.loaderId.empty() ? ToArtifactTypeKey(asset.artifactType) : asset.loaderId,
        asset.name,
        ToArtifactTypeKey(asset.artifactType),
        std::vector<uint8_t>(asset.serializedPayload.begin(), asset.serializedPayload.end())
    });

    NLS::Core::Assets::ArtifactWriter writer(
        GetArtifactStagingRootForAssetPath(record->absolutePath),
        GetArtifactRootForAssetPath(record->absolutePath));
    const auto writeResult = writer.WriteAndCommit(writeRequest, &manifest);
    if (!writeResult.committed || HasErrors(writeResult.diagnostics))
        return false;

    if (!SaveArtifactManifestForAssetPath(record->absolutePath, writeResult.manifest))
        return false;
    m_manifestsBySource[record->id] = writeResult.manifest;
    return true;
}

bool AssetDatabaseFacade::ExtractAsset(
    const AssetDatabaseRecord& asset,
    const std::string& destinationAssetPath)
{
    if (!IsEditorMode())
        return RejectRuntimeEditorApi("AssetDatabase.ExtractAsset");
    if (asset.mainAsset || asset.subAssetKey.empty())
        return false;

    NLS::Core::Assets::ArtifactManifest sourceCopy;
    NLS::Core::Assets::ImportedArtifact sourceArtifact;
    {
        std::lock_guard manifestLock(m_manifestMutex);
        const auto sourceManifest = m_manifestsBySource.find(asset.assetId);
        if (sourceManifest == m_manifestsBySource.end())
            return false;

        const auto* foundArtifact = sourceManifest->second.FindSubAsset(asset.subAssetKey);
        if (!foundArtifact)
            return false;
        sourceArtifact = *foundArtifact;
        sourceCopy = sourceManifest->second;
    }

    const auto destination = ResolveAssetPath(destinationAssetPath);
    if (destination.empty() || std::filesystem::exists(destination) || !IsWritableAssetPath(destination))
        return false;

    AssetObjectRecord object;
    object.name = std::filesystem::path(asset.subAssetKey).filename().generic_string();
    if (const auto colon = asset.subAssetKey.find(':'); colon != std::string::npos)
        object.name = asset.subAssetKey.substr(colon + 1u);
    object.artifactType = sourceArtifact.artifactType;
    object.loaderId = sourceArtifact.loaderId;
    object.serializedPayload = "extracted-from=" + asset.assetPath + "#" + asset.subAssetKey;

    if (!WriteNativeAssetPayload(destination, object, asset.subAssetKey))
        return false;

    auto meta = NLS::Core::Assets::AssetMeta::CreateForAsset(destination);
    if (meta.assetType == NLS::Core::Assets::AssetType::Unknown)
        meta.assetType = NLS::Core::Assets::AssetType::Texture;
    meta.importerId = NLS::Core::Assets::InferImporterId(meta.assetType);
    if (!meta.Save(NLS::Core::Assets::GetAssetMetaPath(destination)))
        return false;

    sourceCopy.subAssets.erase(
        std::remove_if(
            sourceCopy.subAssets.begin(),
            sourceCopy.subAssets.end(),
            [&asset](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return artifact.subAssetKey == asset.subAssetKey;
            }),
        sourceCopy.subAssets.end());
    if (sourceCopy.primarySubAssetKey == asset.subAssetKey)
        sourceCopy.primarySubAssetKey = sourceCopy.subAssets.empty() ? std::string {} : sourceCopy.subAssets.front().subAssetKey;
    {
        std::lock_guard manifestLock(m_manifestMutex);
        m_manifestsBySource[asset.assetId] = std::move(sourceCopy);
    }

    if (!Refresh())
        return false;

    const auto destinationId = ParseAssetId(AssetPathToGUID(destinationAssetPath));
    if (!destinationId.IsValid())
        return false;

    NLS::Core::Assets::ArtifactWriteRequest writeRequest;
    writeRequest.sourceAssetId = destinationId;
    writeRequest.importerId = meta.importerId;
    writeRequest.importerVersion = meta.importerVersion;
    writeRequest.targetPlatform = "editor";
    writeRequest.primarySubAssetKey = asset.subAssetKey;
    writeRequest.artifacts.push_back({
        asset.subAssetKey,
        object.artifactType,
        object.loaderId.empty() ? ToArtifactTypeKey(object.artifactType) : object.loaderId,
        object.name,
        ToArtifactTypeKey(object.artifactType),
        std::vector<uint8_t>(object.serializedPayload.begin(), object.serializedPayload.end())
    });
    NLS::Core::Assets::ArtifactWriter writer(
        GetArtifactStagingRootForAssetPath(destination),
        GetArtifactRootForAssetPath(destination));
    const auto writeResult = writer.WriteAndCommit(writeRequest, nullptr);
    if (!writeResult.committed || HasErrors(writeResult.diagnostics))
        return false;

    if (!SaveArtifactManifestForAssetPath(destination, writeResult.manifest))
        return false;
    AddArtifactManifest(writeResult.manifest);
    return true;
}

bool AssetDatabaseFacade::Contains(const AssetDatabaseRecord& asset) const
{
    if (!IsEditorMode())
        return false;

    std::lock_guard manifestLock(m_manifestMutex);
    const auto path = NormalizeEditorAssetPath(asset.assetPath);
    const auto foundId = m_idByEditorPath.find(path);
    if (foundId == m_idByEditorPath.end() || foundId->second != asset.assetId)
        return false;

    if (asset.subAssetKey.empty())
        return true;

    const auto foundManifest = m_manifestsBySource.find(asset.assetId);
    return foundManifest != m_manifestsBySource.end() &&
        foundManifest->second.FindSubAsset(asset.subAssetKey) != nullptr;
}

bool AssetDatabaseFacade::IsMainAsset(const AssetDatabaseRecord& asset) const
{
    return Contains(asset) && asset.mainAsset;
}

bool AssetDatabaseFacade::IsSubAsset(const AssetDatabaseRecord& asset) const
{
    return Contains(asset) && !asset.mainAsset && !asset.subAssetKey.empty();
}

bool AssetDatabaseFacade::IsForeignAsset(const AssetDatabaseRecord& asset) const
{
    return !Contains(asset);
}

bool AssetDatabaseFacade::IsNativeAsset(const AssetDatabaseRecord& asset) const
{
    return Contains(asset);
}

std::filesystem::path AssetDatabaseFacade::GetArtifactRootForAssetPathForTesting(
    const std::string& assetPath) const
{
    if (!IsEditorMode())
        return {};

    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return {};
    return GetArtifactRootForAssetPath(record->absolutePath);
}

size_t AssetDatabaseFacade::GetQueuedImportCount() const
{
    return m_queuedImports.size();
}

size_t AssetDatabaseFacade::GetCompletedImportCount() const
{
    return m_completedImports;
}

const NLS::Core::Assets::AssetDiagnostics& AssetDatabaseFacade::GetDiagnostics() const
{
    return m_diagnostics;
}

std::filesystem::path AssetDatabaseFacade::ResolveAssetPath(const std::string& assetPath) const
{
    if (!IsEditorMode())
        return {};
    return ResolveEditorAssetPath(m_roots, assetPath);
}

std::string AssetDatabaseFacade::ToEditorAssetPath(const std::filesystem::path& absolutePath) const
{
    return NLS::Editor::Assets::ToEditorAssetPath(m_roots, absolutePath);
}

const NLS::Core::Assets::SourceAssetRecord* AssetDatabaseFacade::FindRecordByEditorAssetPath(
    const std::string& assetPath) const
{
    if (!IsEditorMode())
        return nullptr;

    const auto found = m_idByEditorPath.find(NormalizeEditorAssetPath(assetPath));
    if (found == m_idByEditorPath.end())
        return nullptr;
    return m_sourceDatabase.FindById(found->second);
}

NLS::Core::Assets::AssetId AssetDatabaseFacade::ParseAssetId(const std::string& guid) const
{
    const auto parsed = NLS::Guid::TryParse(guid);
    if (!parsed.has_value())
        return {};
    return NLS::Core::Assets::AssetId(*parsed);
}

std::optional<NLS::Core::Assets::AssetMeta> AssetDatabaseFacade::LoadMetaForPath(
    const std::string& assetPath) const
{
    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return std::nullopt;
    return NLS::Core::Assets::AssetMeta::Load(record->metaPath);
}

bool AssetDatabaseFacade::SaveMetaForPath(
    const std::string& assetPath,
    NLS::Core::Assets::AssetMeta meta)
{
    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return false;
    if (record->readOnly || !IsWritableAssetPath(record->absolutePath))
        return false;
    return meta.Save(record->metaPath);
}

bool AssetDatabaseFacade::RefreshSourceDatabase()
{
    std::vector<NLS::Core::Assets::SourceAssetRoot> scanRoots;
    scanRoots.reserve(m_roots.size());
    for (const auto& root : m_roots)
        scanRoots.push_back({root.path, root.readOnly});

    if (!m_sourceDatabase.ScanRoots(scanRoots))
    {
        m_diagnostics = m_sourceDatabase.GetDiagnostics();
        RebuildPathIndexes();
        return false;
    }

    m_diagnostics = m_sourceDatabase.GetDiagnostics();
    RebuildPathIndexes();
    LoadPersistedArtifactManifests();
    return !HasErrors(m_diagnostics);
}

bool AssetDatabaseFacade::RefreshSingle(
    const std::string& assetPath,
    ImportProgressTracker* progressTracker,
    ImportJobId existingJob,
    const bool refreshDatabase)
{
    if (!IsEditorMode())
        return false;

    const auto absolutePath = ResolveAssetPath(assetPath);
    if (absolutePath.empty())
        return false;
    if (!std::filesystem::exists(absolutePath))
        return false;

    if (refreshDatabase && !Refresh())
        return false;

    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return false;

    const auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return false;

    if (record->assetType == NLS::Core::Assets::AssetType::Prefab)
    {
        std::ifstream input(absolutePath, std::ios::binary);
        if (!input)
            return false;

        const std::string sourceText {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(sourceText, meta->id);
        if (importResult.diagnostics.HasErrors())
            return false;

        const auto artifactRoot = GetArtifactRootForAssetPath(absolutePath);
        const auto stagingRoot = GetArtifactStagingRootForAssetPath(absolutePath);
        if (artifactRoot.empty() ||
            stagingRoot.empty() ||
            !CanSaveArtifactManifestForAssetPath(absolutePath))
        {
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                "assetdatabase-prefab-artifact-manifest-unwritable",
                GetArtifactManifestPathForAssetPath(absolutePath),
                "Imported prefab artifact manifest cannot be written.");
            return false;
        }

        std::optional<NLS::Core::Assets::ArtifactManifest> previousManifest;
        {
            std::lock_guard manifestLock(m_manifestMutex);
            const auto previous = m_manifestsBySource.find(record->id);
            if (previous != m_manifestsBySource.end())
                previousManifest = previous->second;
        }
        const auto editorAssetPath = ToEditorAssetPath(absolutePath);
        const auto primarySubAssetKey = "prefab:" + absolutePath.stem().generic_string();
        ImportJobId job = existingJob;
        if (progressTracker && !job.IsValid())
            job = progressTracker->BeginJob(meta->id, editorAssetPath, "editor", 1u);
        NLS::Core::Assets::ArtifactWriteRequest writeRequest;
        writeRequest.sourceAssetId = meta->id;
        writeRequest.importerId = meta->importerId;
        writeRequest.importerVersion = meta->importerVersion;
        writeRequest.targetPlatform = "editor";
        writeRequest.primarySubAssetKey = primarySubAssetKey;
        writeRequest.artifacts.push_back({
            primarySubAssetKey,
            NLS::Core::Assets::ArtifactType::Prefab,
            "prefab",
            absolutePath.stem().generic_string(),
            "prefab",
            std::vector<uint8_t>(sourceText.begin(), sourceText.end())
        });
        AddNativeAssetManifestDependencies(
            writeRequest.dependencies,
            editorAssetPath,
            ToEditorAssetPath(NLS::Core::Assets::GetAssetMetaPath(absolutePath)),
            absolutePath,
            NLS::Core::Assets::GetAssetMetaPath(absolutePath),
            *meta);

        std::lock_guard publishLock(GetArtifactPublishMutex(artifactRoot));
        ArtifactRootRollback artifactRollback(artifactRoot);
        if (!artifactRollback.Prepare())
        {
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                "assetdatabase-prefab-artifact-rollback-prepare-failed",
                artifactRoot,
                "Previous imported prefab artifact root could not be prepared for rollback.");
            return false;
        }

        NLS::Core::Assets::ArtifactWriter writer(stagingRoot, artifactRoot);
        auto cancellationToken = progressTracker && job.IsValid()
            ? progressTracker->GetCancellationToken(job)
            : std::optional<ImportCancellationTokenHandle> {};
        const auto writeResult = writer.WriteAndCommit(
            writeRequest,
            previousManifest.has_value() ? &*previousManifest : nullptr,
            cancellationToken.has_value() ? &cancellationToken->get() : nullptr);

        m_diagnostics.insert(
            m_diagnostics.end(),
            writeResult.diagnostics.begin(),
            writeResult.diagnostics.end());

        if (!writeResult.committed || HasErrors(writeResult.diagnostics))
        {
            std::error_code restoreError;
            if (!artifactRollback.Restore(&restoreError))
                ReportArtifactRollbackRestoreFailure(m_diagnostics, artifactRoot, restoreError);
            if (progressTracker && job.IsValid())
                progressTracker->FinishJob(job, TerminalStatusForImportFailure(writeResult.diagnostics), writeResult.diagnostics);
            return false;
        }

        if (!SaveArtifactManifestForAssetPath(absolutePath, writeResult.manifest))
        {
            std::error_code restoreError;
            if (!artifactRollback.Restore(&restoreError))
                ReportArtifactRollbackRestoreFailure(m_diagnostics, artifactRoot, restoreError);
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                "assetdatabase-prefab-artifact-manifest-write-failed",
                GetArtifactManifestPathForAssetPath(absolutePath),
                "Imported prefab artifact manifest could not be written.");
            if (progressTracker && job.IsValid())
                progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
            return false;
        }

        AddArtifactManifest(writeResult.manifest);
        artifactRollback.Commit();
        if (progressTracker && job.IsValid())
            progressTracker->FinishJob(job, ImportJobTerminalStatus::Succeeded, writeResult.diagnostics);
        return true;
    }

    if (record->assetType == NLS::Core::Assets::AssetType::Shader)
    {
        const auto artifactRoot = GetArtifactRootForAssetPath(absolutePath);
        const auto stagingRoot = GetArtifactStagingRootForAssetPath(absolutePath);
        const auto editorAssetPath = ToEditorAssetPath(absolutePath);
        const auto subAssetKey = "shader:" + absolutePath.stem().generic_string();
        const auto* assetRoot = FindEditorAssetRootForAbsolutePath(m_roots, absolutePath);

        if (artifactRoot.empty() ||
            stagingRoot.empty() ||
            !CanSaveArtifactManifestForAssetPath(absolutePath))
        {
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                "assetdatabase-shader-artifact-manifest-unwritable",
                GetArtifactManifestPathForAssetPath(absolutePath),
                "Imported shader artifact manifest cannot be written.");
            return false;
        }

        std::optional<NLS::Core::Assets::ArtifactManifest> previousManifest;
        {
            std::lock_guard manifestLock(m_manifestMutex);
            const auto previous = m_manifestsBySource.find(record->id);
            if (previous != m_manifestsBySource.end())
                previousManifest = previous->second;
        }
        ImportJobId job = existingJob;
        if (progressTracker && !job.IsValid())
            job = progressTracker->BeginJob(meta->id, editorAssetPath, "editor", 1u);
        if (progressTracker && job.IsValid())
            progressTracker->ReportProgress(job, ImportPhase::SourceParse, 0.05, "Compiling shader source");

        std::vector<ImportedShaderArtifactPayload> shaderArtifacts;
        if (ToLower(absolutePath.extension().generic_string()) == ".shader")
        {
            auto shaderLabImport = ImportShaderLabArtifactPayloads(
                absolutePath,
                editorAssetPath,
                subAssetKey,
                assetRoot);
            if (!shaderLabImport.Succeeded())
            {
                AddDiagnostic(
                    NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                    "assetdatabase-shaderlab-import-failed",
                    absolutePath,
                    shaderLabImport.diagnostic);
                if (progressTracker && job.IsValid())
                    progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
                return false;
            }
            shaderArtifacts = std::move(shaderLabImport.payloads);
        }
        else
        {
            ImportedShaderArtifactPayload payload;
            payload.subAssetKey = subAssetKey;
            payload.displayName = absolutePath.stem().generic_string();
            payload.artifact = ImportShaderArtifactPayload(
                absolutePath,
                editorAssetPath,
                subAssetKey,
                assetRoot);
            if (!NLS::Render::Assets::HasUsableShaderArtifactStage(payload.artifact) ||
                HasFailedShaderArtifactStage(payload.artifact))
            {
                std::ostringstream diagnostic;
                diagnostic << absolutePath.generic_string()
                    << ": shader import did not produce any usable shader stage.";
                for (const auto& stage : payload.artifact.stages)
                {
                    if (!stage.output.diagnostics.empty())
                        diagnostic << "\n" << stage.output.diagnostics;
                }
                AddDiagnostic(
                    NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                    "assetdatabase-shader-import-failed",
                    absolutePath,
                    diagnostic.str());
                if (progressTracker && job.IsValid())
                    progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
                return false;
            }
            shaderArtifacts.push_back(std::move(payload));
        }

        NLS::Core::Assets::ArtifactWriteRequest writeRequest;
        writeRequest.sourceAssetId = meta->id;
        writeRequest.importerId = meta->importerId;
        writeRequest.importerVersion = meta->importerVersion;
        writeRequest.targetPlatform = "editor";
        writeRequest.primarySubAssetKey = subAssetKey;
        for (const auto& shaderArtifact : shaderArtifacts)
        {
            writeRequest.artifacts.push_back({
                shaderArtifact.subAssetKey,
                NLS::Core::Assets::ArtifactType::Shader,
                "shader",
                shaderArtifact.displayName,
                "shader",
                NLS::Render::Assets::SerializeShaderArtifact(shaderArtifact.artifact)
            });
        }
        AddNativeAssetManifestDependencies(
            writeRequest.dependencies,
            editorAssetPath,
            ToEditorAssetPath(NLS::Core::Assets::GetAssetMetaPath(absolutePath)),
            absolutePath,
            NLS::Core::Assets::GetAssetMetaPath(absolutePath),
            *meta);
        for (const auto& shaderArtifact : shaderArtifacts)
        {
            AddShaderStageSourceDependencies(
                writeRequest.dependencies,
                m_roots,
                absolutePath,
                shaderArtifact.artifact);
        }
        AddUniqueDependency(writeRequest.dependencies, BuildShaderCompilerToolchainDependency());

        if (progressTracker && job.IsValid())
            progressTracker->ReportProgress(job, ImportPhase::ArtifactWrite, 0.85, "Writing shader artifact");

        std::lock_guard publishLock(GetArtifactPublishMutex(artifactRoot));
        ArtifactRootRollback artifactRollback(artifactRoot);
        if (!artifactRollback.Prepare())
        {
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                "assetdatabase-shader-artifact-rollback-prepare-failed",
                artifactRoot,
                "Previous imported shader artifact root could not be prepared for rollback.");
            if (progressTracker && job.IsValid())
                progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
            return false;
        }

        NLS::Core::Assets::ArtifactWriter writer(stagingRoot, artifactRoot);
        auto cancellationToken = progressTracker && job.IsValid()
            ? progressTracker->GetCancellationToken(job)
            : std::optional<ImportCancellationTokenHandle> {};
        const auto writeResult = writer.WriteAndCommit(
            writeRequest,
            previousManifest.has_value() ? &*previousManifest : nullptr,
            cancellationToken.has_value() ? &cancellationToken->get() : nullptr);

        m_diagnostics.insert(
            m_diagnostics.end(),
            writeResult.diagnostics.begin(),
            writeResult.diagnostics.end());

        if (!writeResult.committed || HasErrors(writeResult.diagnostics))
        {
            std::error_code restoreError;
            if (!artifactRollback.Restore(&restoreError))
                ReportArtifactRollbackRestoreFailure(m_diagnostics, artifactRoot, restoreError);
            if (progressTracker && job.IsValid())
                progressTracker->FinishJob(job, TerminalStatusForImportFailure(writeResult.diagnostics), writeResult.diagnostics);
            return false;
        }

        if (!SaveArtifactManifestForAssetPath(absolutePath, writeResult.manifest))
        {
            std::error_code restoreError;
            if (!artifactRollback.Restore(&restoreError))
                ReportArtifactRollbackRestoreFailure(m_diagnostics, artifactRoot, restoreError);
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                "assetdatabase-shader-artifact-manifest-write-failed",
                GetArtifactManifestPathForAssetPath(absolutePath),
                "Imported shader artifact manifest could not be written.");
            if (progressTracker && job.IsValid())
                progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
            return false;
        }

        AddArtifactManifest(writeResult.manifest);
        artifactRollback.Commit();
        if (progressTracker && job.IsValid())
            progressTracker->FinishJob(job, ImportJobTerminalStatus::Succeeded, writeResult.diagnostics);
        return true;
    }

    if (record->assetType != NLS::Core::Assets::AssetType::ModelScene)
        return true;

    const auto modelSourceAssetId = record->id;
    const auto sceneKey = absolutePath.stem().generic_string();
    const auto artifactRoot = GetArtifactRootForAssetPath(absolutePath);
    const auto stagingRoot = GetArtifactStagingRootForAssetPath(absolutePath);
    const auto editorAssetPath = ToEditorAssetPath(absolutePath);
    const auto* assetRoot = FindEditorAssetRootForAbsolutePath(m_roots, absolutePath);
    auto sourceParent = std::filesystem::path(editorAssetPath).parent_path();
    if (!sourceParent.empty() && *sourceParent.begin() == "Assets")
        sourceParent = sourceParent.lexically_relative("Assets");

    if (!CanSaveArtifactManifestForAssetPath(absolutePath))
    {
        AddDiagnostic(
            NLS::Core::Assets::AssetDiagnosticSeverity::Error,
            "assetdatabase-artifact-manifest-unwritable",
            GetArtifactManifestPathForAssetPath(absolutePath),
            "Imported artifact manifest cannot be written.");
        return false;
    }

    std::optional<NLS::Core::Assets::ArtifactManifest> previousManifest;
    std::optional<std::string> materialShaderResourcePath;
    auto refreshModelMaterialShaderDependency =
        [&]()
    {
        std::lock_guard manifestLock(m_manifestMutex);
        const auto previous = m_manifestsBySource.find(modelSourceAssetId);
        if (previous != m_manifestsBySource.end())
            previousManifest = previous->second;

        materialShaderResourcePath = FindImportedShaderArtifactResourcePath(
            m_manifestsBySource,
            m_editorPathById,
            kStandardPbrShaderAssetPath);
    };
    refreshModelMaterialShaderDependency();

    if (!materialShaderResourcePath.has_value() &&
        !std::filesystem::is_regular_file(ResolveAssetPath(kStandardPbrShaderAssetPath)))
    {
        if (EnsureStandardPbrShaderLabSourceAvailable())
        {
            if (!RefreshSourceDatabase())
            {
                AddDiagnostic(
                    NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                    "assetdatabase-standard-pbr-refresh-failed",
                    kStandardPbrShaderAssetPath,
                    "Model material import could not refresh the synchronized StandardPBR ShaderLab source.");
                return false;
            }
        }
    }
    if (!materialShaderResourcePath.has_value() && !ResolveAssetPath(kStandardPbrShaderAssetPath).empty())
    {
        const bool importedStandardPbr =
            ImportAssetImmediateInternal(kStandardPbrShaderAssetPath, false);
        if (importedStandardPbr)
            refreshModelMaterialShaderDependency();
    }
    if (!materialShaderResourcePath.has_value())
    {
        AddDiagnostic(
            NLS::Core::Assets::AssetDiagnosticSeverity::Error,
            "assetdatabase-model-material-shader-missing",
            absolutePath,
            "Model material import requires imported ShaderLab artifact Assets/Engine/Shaders/ShaderLab/StandardPBR.shader.");
        return false;
    }
    ImportJobId job = existingJob;
    if (progressTracker && !job.IsValid())
        job = progressTracker->BeginJob(meta->id, editorAssetPath, "editor", 1u);
    if (progressTracker && job.IsValid())
        progressTracker->ReportProgress(job, ImportPhase::SourceParse, 0.05, "Scanning source asset");

    std::lock_guard publishLock(GetArtifactPublishMutex(artifactRoot));
    ArtifactRootRollback artifactRollback(artifactRoot);
    if (!artifactRollback.Prepare())
    {
        AddDiagnostic(
            NLS::Core::Assets::AssetDiagnosticSeverity::Error,
            "assetdatabase-artifact-rollback-prepare-failed",
            artifactRoot,
            "Previous imported artifact root could not be prepared for rollback.");
        if (progressTracker && job.IsValid())
            progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
        return false;
    }

    const auto importResult = ImportExternalModelAsset({
        absolutePath,
        stagingRoot,
        artifactRoot,
        *meta,
        sceneKey,
        "editor",
        previousManifest.has_value() ? &*previousManifest : nullptr,
        progressTracker,
        job,
        sourceParent,
        assetRoot ? GetEditorAssetRootLibraryPath(*assetRoot).parent_path() : std::filesystem::path {},
        kStandardPbrShaderAssetPath
    });

    m_diagnostics.insert(
        m_diagnostics.end(),
        importResult.diagnostics.begin(),
        importResult.diagnostics.end());

    if (!importResult.imported)
    {
        std::error_code restoreError;
        if (!artifactRollback.Restore(&restoreError))
            ReportArtifactRollbackRestoreFailure(m_diagnostics, artifactRoot, restoreError);
        if (progressTracker && job.IsValid())
            progressTracker->FinishJob(job, TerminalStatusForImportFailure(importResult.diagnostics), importResult.diagnostics);
        return false;
    }

    auto committedManifest = importResult.manifest;
    AddNativeAssetManifestDependencies(
        committedManifest.dependencies,
        editorAssetPath,
        ToEditorAssetPath(NLS::Core::Assets::GetAssetMetaPath(absolutePath)),
        absolutePath,
        NLS::Core::Assets::GetAssetMetaPath(absolutePath),
        *meta);

    if (!SaveArtifactManifestForAssetPath(absolutePath, committedManifest))
    {
        std::error_code restoreError;
        if (!artifactRollback.Restore(&restoreError))
            ReportArtifactRollbackRestoreFailure(m_diagnostics, artifactRoot, restoreError);
        if (progressTracker && job.IsValid())
            progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, importResult.diagnostics);
        return false;
    }

    AddArtifactManifest(committedManifest);
    artifactRollback.Commit();
    if (progressTracker && job.IsValid())
        progressTracker->FinishJob(job, ImportJobTerminalStatus::Succeeded, importResult.diagnostics);
    return true;
}

std::filesystem::path AssetDatabaseFacade::ResolveArtifactPathForRecord(
    const NLS::Core::Assets::SourceAssetRecord& record,
    const std::string& artifactPath) const
{
    if (artifactPath.empty())
        return {};
    const auto path = std::filesystem::path(artifactPath).lexically_normal();
    if (!NLS::Core::Assets::IsArtifactStorageFileName(path.filename().generic_string()))
        return {};

    if (artifactPath == ToEditorAssetPath(record.absolutePath))
        return record.absolutePath;

    const auto artifactRoot = GetArtifactRootForAssetPath(record.absolutePath);
    if (artifactRoot.empty())
        return {};

    std::vector<std::filesystem::path> candidates;
    if (path.is_absolute())
    {
        candidates.push_back(NLS::Core::Assets::NormalizeAssetPath(path));
    }
    else
    {
        candidates.push_back(NLS::Core::Assets::NormalizeAssetPath(artifactRoot / path));
        if (const auto* root = FindEditorAssetRootForAbsolutePath(m_roots, record.absolutePath))
        {
            const auto projectRelative =
                NLS::Core::Assets::NormalizeAssetPath(GetEditorAssetRootLibraryPath(*root).parent_path() / path);
            if (std::find(candidates.begin(), candidates.end(), projectRelative) == candidates.end())
                candidates.push_back(projectRelative);
        }
    }

    for (const auto& candidate : candidates)
    {
        if (IsPhysicalRegularFileInsideEditorAssetRoot(candidate, artifactRoot))
        {
            return candidate;
        }
    }

    return {};
}

std::filesystem::path AssetDatabaseFacade::GetArtifactRootForAssetPath(
    const std::filesystem::path& absolutePath) const
{
    const auto* root = FindEditorAssetRootForAbsolutePath(m_roots, absolutePath);
    if (!root)
        return {};
    return GetEditorAssetRootLibraryPath(*root) / "Artifacts";
}

std::filesystem::path AssetDatabaseFacade::GetArtifactStagingRootForAssetPath(
    const std::filesystem::path& absolutePath) const
{
    const auto* root = FindEditorAssetRootForAbsolutePath(m_roots, absolutePath);
    if (!root)
        return {};
    const auto assetPath = ToEditorAssetPath(absolutePath);
    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record || !record->id.IsValid())
        return {};
    return GetEditorAssetRootLibraryPath(*root) / "ArtifactStaging" / record->id.ToString();
}

std::filesystem::path AssetDatabaseFacade::GetArtifactManifestPathForAssetPath(
    const std::filesystem::path& absolutePath) const
{
    (void)absolutePath;
    return {};
}

std::filesystem::path AssetDatabaseFacade::GetArtifactDatabasePathForAssetPath(
    const std::filesystem::path& absolutePath) const
{
    const auto* root = FindEditorAssetRootForAbsolutePath(m_roots, absolutePath);
    if (!root)
        return {};
    return GetEditorAssetRootLibraryPath(*root) / "ArtifactDB";
}

void AssetDatabaseFacade::LoadPersistedArtifactManifests()
{
    std::unordered_map<NLS::Core::Assets::AssetId, NLS::Core::Assets::ArtifactManifest> loadedManifests;
    std::vector<NLS::Core::Assets::AssetId> visitedSourceIds;
    std::unordered_map<std::string, NLS::Core::Assets::ArtifactDatabase> databasesByPath;
    for (const auto& record : m_sourceDatabase.GetRecords())
    {
        if (record.assetType != NLS::Core::Assets::AssetType::ModelScene &&
            record.assetType != NLS::Core::Assets::AssetType::Prefab &&
            record.assetType != NLS::Core::Assets::AssetType::Shader)
        {
            continue;
        }

        visitedSourceIds.push_back(record.id);

        const auto databasePath = GetArtifactDatabasePathForAssetPath(record.absolutePath);
        if (databasePath.empty())
            continue;

        const auto databaseKey = databasePath.lexically_normal().generic_string();
        auto [databaseIt, inserted] = databasesByPath.try_emplace(databaseKey);
        if (inserted && !std::filesystem::exists(databasePath))
            continue;
        if (inserted && !databaseIt->second.Load(databasePath))
        {
            auto message = std::string("ArtifactDB could not be read; reimport source assets.");
            if (!databaseIt->second.GetLastError().empty())
                message += " " + databaseIt->second.GetLastError();
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Warning,
                "assetdatabase-artifactdb-read-failed",
                databasePath,
                std::move(message));
            continue;
        }

        auto manifest = databaseIt->second.BuildManifestForSource(record.id);
        if (!manifest.has_value())
            continue;

        if (manifest->sourceAssetId != record.id)
        {
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Warning,
                "assetdatabase-artifact-manifest-source-mismatch",
                databasePath,
                "ArtifactDB record belongs to a different source asset id.");
            continue;
        }

        const auto originalSubAssetCount = manifest->subAssets.size();
        auto filteredManifest = FilterContentStorageArtifacts(std::move(*manifest));
        if (filteredManifest.subAssets.size() != originalSubAssetCount)
        {
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Warning,
                "assetdatabase-artifact-manifest-invalid-payload-path",
                databasePath,
                "ArtifactDB contains a non-content-addressed payload path; reimport the source asset.");
            continue;
        }

        loadedManifests[filteredManifest.sourceAssetId] = std::move(filteredManifest);
    }

    {
        std::lock_guard manifestLock(m_manifestMutex);
        for (const auto& sourceId : visitedSourceIds)
        {
            m_manifestsBySource.erase(sourceId);
            const auto path = m_editorPathById.find(sourceId);
            if (path != m_editorPathById.end())
                RemoveKnownCurrentArtifactManifestSnapshotForAssetPathLocked(path->second);
        }
        for (auto& [sourceId, manifest] : loadedManifests)
            m_manifestsBySource[sourceId] = std::move(manifest);
    }

    RefreshKnownCurrentArtifactManifestSnapshot();
}

void AssetDatabaseFacade::RefreshKnownCurrentArtifactManifestSnapshot()
{
    struct Candidate
    {
        std::string assetPath;
        NLS::Core::Assets::AssetId sourceId;
    };

    std::vector<Candidate> candidates;
    {
        std::lock_guard manifestLock(m_manifestMutex);
        candidates.reserve(m_manifestsBySource.size());
        for (const auto& [sourceId, _] : m_manifestsBySource)
        {
            const auto found = m_editorPathById.find(sourceId);
            if (found != m_editorPathById.end())
                candidates.push_back({found->second, sourceId});
        }
    }

    std::unordered_set<std::string> knownCurrentPaths;
    std::vector<ObjectReferencePickerAssetSnapshot> snapshots;
    for (const auto& candidate : candidates)
    {
        const auto normalized = NormalizeEditorAssetPath(candidate.assetPath);
        if (normalized.empty() || !IsArtifactManifestCurrentForAssetPath(normalized))
            continue;

        std::optional<NLS::Core::Assets::ArtifactManifest> manifest;
        {
            std::lock_guard manifestLock(m_manifestMutex);
            const auto found = m_manifestsBySource.find(candidate.sourceId);
            if (found != m_manifestsBySource.end())
                manifest = found->second;
        }
        if (!manifest.has_value())
            continue;

        knownCurrentPaths.insert(normalized);
        auto snapshot = BuildObjectReferencePickerAssetSnapshot(normalized, *manifest);
        if (!snapshot.subAssets.empty())
            snapshots.push_back(std::move(snapshot));
    }

    std::lock_guard manifestLock(m_manifestMutex);
    ReplaceKnownCurrentArtifactManifestSnapshotsLocked(std::move(knownCurrentPaths), std::move(snapshots));
}

void AssetDatabaseFacade::UpdateKnownCurrentArtifactManifestForAssetPath(const std::string& assetPath)
{
    const auto normalized = NormalizeEditorAssetPath(assetPath);
    if (normalized.empty())
        return;

    if (IsArtifactManifestCurrentForAssetPath(normalized))
    {
        const auto* record = FindRecordByEditorAssetPath(normalized);
        if (!record)
            return;

        std::lock_guard manifestLock(m_manifestMutex);
        const auto manifest = m_manifestsBySource.find(record->id);
        if (manifest != m_manifestsBySource.end())
            PublishKnownCurrentArtifactManifestSnapshotForAssetPathLocked(normalized, manifest->second);
    }
    else
    {
        std::lock_guard manifestLock(m_manifestMutex);
        RemoveKnownCurrentArtifactManifestSnapshotForAssetPathLocked(normalized);
    }
}

bool AssetDatabaseFacade::CanSaveArtifactManifestForAssetPath(const std::filesystem::path& absolutePath) const
{
    const auto databasePath = GetArtifactDatabasePathForAssetPath(absolutePath);
    if (databasePath.empty())
        return false;

    std::error_code error;
    if (std::filesystem::exists(databasePath, error) && !std::filesystem::is_directory(databasePath, error))
        return false;

    const auto parentPath = databasePath.parent_path();
    if (parentPath.empty())
        return false;

    if (std::filesystem::exists(parentPath, error))
        return std::filesystem::is_directory(parentPath, error);

    return true;
}

bool AssetDatabaseFacade::SaveArtifactManifestForAssetPath(
    const std::filesystem::path& absolutePath,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    const auto databasePath = GetArtifactDatabasePathForAssetPath(absolutePath);
    if (databasePath.empty())
        return false;

    auto sourcePath = ToEditorAssetPath(absolutePath);
    if (sourcePath.empty())
        return false;

    const auto databaseKey = databasePath.lexically_normal().generic_string();
    bool shouldSaveImmediately = false;
    {
        std::lock_guard databaseLock(GetArtifactDatabaseMutex(databasePath));
        std::lock_guard cacheLock(m_artifactDatabaseCacheMutex);
        auto [database, inserted] = m_artifactDatabasesByPath.try_emplace(databaseKey);
        auto& artifactDatabase = database->second;
        if (inserted && std::filesystem::exists(databasePath) && !artifactDatabase.Load(databasePath))
            artifactDatabase.Clear();

        auto indexedManifest = manifest;
        const auto* root = FindEditorAssetRootForAbsolutePath(m_roots, absolutePath);
        if (root)
        {
            const auto artifactPathBase = GetEditorAssetRootLibraryPath(*root).parent_path();
            for (auto& artifact : indexedManifest.subAssets)
            {
                const auto artifactPath = NLS::Core::Assets::NormalizeAssetPath(artifact.artifactPath);
                if (artifactPath.is_absolute())
                {
                    std::error_code error;
                    const auto relative = std::filesystem::relative(artifactPath, artifactPathBase, error);
                    if (!error && !relative.empty() && IsPathInsideEditorAssetRoot(artifactPath, artifactPathBase))
                        artifact.artifactPath = relative.generic_string();
                }
            }
        }

        artifactDatabase.UpsertManifest(
            indexedManifest,
            sourcePath,
            NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
        m_dirtyArtifactDatabasePaths.insert(databaseKey);
        shouldSaveImmediately = !m_assetEditing;
    }

    return !shouldSaveImmediately || FlushArtifactDatabaseCache();
}

bool AssetDatabaseFacade::SaveArtifactDatabaseManifest(const NLS::Core::Assets::ArtifactManifest& manifest)
{
    const auto path = m_editorPathById.find(manifest.sourceAssetId);
    if (path == m_editorPathById.end())
        return false;

    const auto absolutePath = ResolveAssetPath(path->second);
    if (absolutePath.empty())
        return false;

    return SaveArtifactManifestForAssetPath(absolutePath, manifest);
}

bool AssetDatabaseFacade::FlushArtifactDatabaseCache()
{
    std::vector<std::string> dirtyDatabasePaths;
    {
        std::lock_guard cacheLock(m_artifactDatabaseCacheMutex);
        dirtyDatabasePaths.assign(m_dirtyArtifactDatabasePaths.begin(), m_dirtyArtifactDatabasePaths.end());
    }

    bool ok = true;
    for (const auto& databaseKey : dirtyDatabasePaths)
    {
        const std::filesystem::path databasePath(databaseKey);
        std::lock_guard databaseLock(GetArtifactDatabaseMutex(databasePath));
        NLS::Core::Assets::ArtifactDatabase artifactDatabase;
        {
            std::lock_guard cacheLock(m_artifactDatabaseCacheMutex);
            if (m_dirtyArtifactDatabasePaths.find(databaseKey) == m_dirtyArtifactDatabasePaths.end())
                continue;

            const auto found = m_artifactDatabasesByPath.find(databaseKey);
            if (found == m_artifactDatabasesByPath.end())
            {
                m_dirtyArtifactDatabasePaths.erase(databaseKey);
                continue;
            }

            artifactDatabase = found->second;
        }
        const bool saved = artifactDatabase.Save(databasePath);
        ok = saved && ok;
        if (saved)
        {
            std::lock_guard cacheLock(m_artifactDatabaseCacheMutex);
            m_dirtyArtifactDatabasePaths.erase(databaseKey);
        }
        else
        {
            auto message = std::string("ArtifactDB could not be saved.");
            if (!artifactDatabase.GetLastError().empty())
                message += " " + artifactDatabase.GetLastError();
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                "assetdatabase-artifactdb-save-failed",
                databasePath,
                std::move(message));
        }
    }
    return ok;
}

std::string AssetDatabaseFacade::MakeSubAssetKey(const AssetObjectRecord& asset) const
{
    return ToArtifactTypeKey(asset.artifactType) + ":" + asset.name;
}

NLS::Core::Assets::ImportedArtifact AssetDatabaseFacade::MakeImportedArtifact(
    NLS::Core::Assets::AssetId owner,
    const AssetObjectRecord& asset,
    const std::string& subAssetKey,
    const std::filesystem::path& artifactPath) const
{
    return {
        owner,
        subAssetKey,
        asset.artifactType,
        asset.loaderId.empty() ? ToArtifactTypeKey(asset.artifactType) : asset.loaderId,
        "editor",
        ToEditorAssetPath(artifactPath),
        "sha256:" + owner.ToString() + ":" + subAssetKey,
        asset.name
    };
}

bool AssetDatabaseFacade::WriteNativeAssetPayload(
    const std::filesystem::path& absolutePath,
    const AssetObjectRecord& asset,
    const std::string& subAssetKey) const
{
    std::error_code error;
    std::filesystem::create_directories(absolutePath.parent_path(), error);
    if (error)
        return false;

    std::vector<uint8_t> payload(asset.serializedPayload.begin(), asset.serializedPayload.end());
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = asset.artifactType;
    metadata.schemaName = asset.loaderId.empty() ? ToArtifactTypeKey(asset.artifactType) : asset.loaderId;
    metadata.schemaVersion = 1u;
    metadata.subAssetKey = subAssetKey;
    metadata.displayName = asset.name;
    metadata.importerId = metadata.schemaName;
    metadata.importerVersion = 1u;
    metadata.targetPlatform = "editor";

    const auto bytes = NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
    std::ofstream output(absolutePath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool AssetDatabaseFacade::IsWritableAssetPath(const std::filesystem::path& absolutePath) const
{
    if (absolutePath.empty())
        return false;
    return IsEditorAssetPathWritable(m_roots, absolutePath);
}

bool AssetDatabaseFacade::IsMutableAssetRecord(const std::string& assetPath) const
{
    const auto* record = FindRecordByEditorAssetPath(assetPath);
    return record && !record->readOnly && IsWritableAssetPath(record->absolutePath);
}

void AssetDatabaseFacade::RebuildPathIndexes()
{
    m_idByEditorPath.clear();
    m_editorPathById.clear();

    for (const auto& record : m_sourceDatabase.GetRecords())
    {
        const auto editorAssetPath = ToEditorAssetPath(record.absolutePath);
        if (editorAssetPath.empty())
            continue;

        if (m_idByEditorPath.find(editorAssetPath) != m_idByEditorPath.end())
        {
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                "assetdatabase-editor-path-alias",
                record.absolutePath,
                "Multiple mounted asset roots produced the same editor asset path.");
            continue;
        }

        m_idByEditorPath[editorAssetPath] = record.id;
        m_editorPathById[record.id] = editorAssetPath;
    }
}

void AssetDatabaseFacade::AddDiagnostic(
    const NLS::Core::Assets::AssetDiagnosticSeverity severity,
    std::string code,
    std::filesystem::path path,
    std::string message)
{
    m_diagnostics.push_back({
        severity,
        std::move(code),
        {},
        std::move(path),
        std::move(message)
    });
}

bool AssetDatabaseFacade::RejectRuntimeEditorApi(std::string apiName)
{
    AddDiagnostic(
        NLS::Core::Assets::AssetDiagnosticSeverity::Error,
        "assetdatabase-editor-api-unavailable-at-runtime",
        {},
        std::move(apiName));
    return false;
}

bool AssetDatabaseFacade::IsEditorMode() const
{
    return m_mode == AssetDatabaseAccessMode::Editor;
}
}
