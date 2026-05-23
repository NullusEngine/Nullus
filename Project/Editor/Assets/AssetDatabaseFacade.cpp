#include "Assets/AssetDatabaseFacade.h"

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactWriter.h"
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/EditorAssetManifestJson.h"
#include "Assets/AssetPath.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/ExternalAssetImporter.h"
#include "Guid.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Resources/ShaderType.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"

#include <Json/json.hpp>

#include <algorithm>
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
    std::string targetProfile)
{
    NLS::Render::ShaderCompiler::ShaderCompileOptions options;
    options.sourceLanguage = NLS::Render::ShaderCompiler::ShaderSourceLanguage::HLSL;
    options.targetPlatform = targetPlatform;
    options.entryPoint = std::move(entryPoint);
    options.targetProfile = std::move(targetProfile);
    options.includeDirectories.push_back(sourcePath.parent_path().string());
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
    NLS::Render::ShaderCompiler::ShaderCompilationOutput output)
{
    NormalizeShaderImportOutput(output);

    NLS::Render::Assets::ShaderArtifactStage stage;
    stage.stage = input.stage;
    stage.targetPlatform = input.options.targetPlatform;
    stage.entryPoint = input.options.entryPoint;
    stage.targetProfile = input.options.targetProfile;
    stage.output = std::move(output);
    return stage;
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
    std::vector<NLS::Render::ShaderCompiler::ShaderCompilationInput> inputs;
    if (HasEntryPointToken(sourceText, "VSMain"))
    {
        inputs.push_back(MakeShaderCompilationInput(sourcePath, ShaderStage::Vertex, ShaderTargetPlatform::DXIL, "VSMain", "vs_6_0"));
        inputs.push_back(MakeShaderCompilationInput(sourcePath, ShaderStage::Vertex, ShaderTargetPlatform::SPIRV, "VSMain", "vs_6_0"));
    }
    if (HasEntryPointToken(sourceText, "PSMain"))
    {
        inputs.push_back(MakeShaderCompilationInput(sourcePath, ShaderStage::Pixel, ShaderTargetPlatform::DXIL, "PSMain", "ps_6_0"));
        inputs.push_back(MakeShaderCompilationInput(sourcePath, ShaderStage::Pixel, ShaderTargetPlatform::SPIRV, "PSMain", "ps_6_0"));
    }
    if (HasEntryPointToken(sourceText, "CSMain"))
    {
        inputs.push_back(MakeShaderCompilationInput(sourcePath, ShaderStage::Compute, ShaderTargetPlatform::DXIL, "CSMain", "cs_6_0"));
        inputs.push_back(MakeShaderCompilationInput(sourcePath, ShaderStage::Compute, ShaderTargetPlatform::SPIRV, "CSMain", "cs_6_0"));
    }

    NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    compiler.SetCacheDatabasePath(ShaderCacheDatabasePathForAssetRoot(assetRoot));
    const auto outputs = compiler.CompileBatch(inputs);
    artifact.stages.reserve(outputs.size());
    for (size_t index = 0u; index < outputs.size() && index < inputs.size(); ++index)
        artifact.stages.push_back(MakeShaderArtifactStage(inputs[index], outputs[index]));
    NLS::Render::Assets::AppendGlslShaderArtifactStages(artifact);

    std::vector<NLS::Render::ShaderCompiler::ShaderReflectionInput> reflectionInputs;
    reflectionInputs.reserve(inputs.size());
    for (size_t index = 0u; index < artifact.stages.size() && index < inputs.size(); ++index)
    {
        const auto& stage = artifact.stages[index];
        if (stage.output.status == NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
            !stage.output.bytecode.empty())
        {
            reflectionInputs.push_back({inputs[index], stage.output});
        }
    }

    for (const auto& reflectedStage : compiler.ReflectBatch(reflectionInputs))
    {
        for (const auto& constantBuffer : reflectedStage.constantBuffers)
        {
            const auto exists = std::any_of(
                artifact.reflection.constantBuffers.begin(),
                artifact.reflection.constantBuffers.end(),
                [&constantBuffer](const NLS::Render::Resources::ShaderConstantBufferDesc& existing)
                {
                    return existing.name == constantBuffer.name &&
                        existing.bindingSpace == constantBuffer.bindingSpace &&
                        existing.bindingIndex == constantBuffer.bindingIndex;
                });
            if (!exists)
                artifact.reflection.constantBuffers.push_back(constantBuffer);
        }

        for (const auto& property : reflectedStage.properties)
        {
            const auto exists = std::any_of(
                artifact.reflection.properties.begin(),
                artifact.reflection.properties.end(),
                [&property](const NLS::Render::Resources::ShaderPropertyDesc& existing)
                {
                    return existing.name == property.name &&
                        existing.kind == property.kind &&
                        existing.bindingSpace == property.bindingSpace &&
                        existing.bindingIndex == property.bindingIndex &&
                        existing.parentConstantBuffer == property.parentConstantBuffer;
                });
            if (!exists)
                artifact.reflection.properties.push_back(property);
        }
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

std::optional<std::string> FindImportedMaterialShaderArtifactResourcePath(
    const std::unordered_map<NLS::Core::Assets::AssetId, NLS::Core::Assets::ArtifactManifest>& manifestsBySource,
    const std::unordered_map<NLS::Core::Assets::AssetId, std::string>& editorPathById)
{
    const auto materialShaderTypes = NLS::Render::Resources::GetShaderTypeRegistry().FindByName("StandardPBRPS");
    if (!materialShaderTypes)
        return std::nullopt;

    const auto sourcePath = std::filesystem::path(materialShaderTypes->GetSourcePath());
    const auto assetsMarker = std::find(sourcePath.begin(), sourcePath.end(), std::filesystem::path("Assets"));
    if (assetsMarker == sourcePath.end())
        return std::nullopt;

    std::filesystem::path editorAssetPath;
    for (auto it = assetsMarker; it != sourcePath.end(); ++it)
        editorAssetPath /= *it;

    return FindImportedShaderArtifactResourcePath(
        manifestsBySource,
        editorPathById,
        editorAssetPath.generic_string());
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
        std::filesystem::remove_all(m_backupRoot, error);
        error.clear();

        if (!std::filesystem::exists(m_artifactRoot, error))
            return true;

        std::filesystem::rename(m_artifactRoot, m_backupRoot, error);
        if (!error)
        {
            m_hasBackup = true;
            return true;
        }

        return false;
    }

    bool Restore(std::error_code* restoreError = nullptr)
    {
        std::error_code error;
        std::filesystem::remove_all(m_artifactRoot, error);
        if (error)
        {
            if (restoreError)
                *restoreError = error;
            return false;
        }

        error.clear();
        if (m_hasBackup)
        {
            std::filesystem::rename(m_backupRoot, m_artifactRoot, error);
            if (error)
            {
                if (restoreError)
                    *restoreError = error;
                return false;
            }
        }
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
    default: return "asset";
    }
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
    default:
        return {};
    }
}

std::string ToDependencyKindKey(const NLS::Core::Assets::AssetDependencyKind kind)
{
    using NLS::Core::Assets::AssetDependencyKind;
    switch (kind)
    {
    case AssetDependencyKind::SourceFileHash: return "source-file-hash";
    case AssetDependencyKind::SourceAssetGuid: return "source-asset-guid";
    case AssetDependencyKind::ImportedArtifact: return "imported-artifact";
    case AssetDependencyKind::PathToGuidMapping: return "path-to-guid-mapping";
    case AssetDependencyKind::BuildTarget: return "build-target";
    case AssetDependencyKind::ImporterVersion: return "importer-version";
    case AssetDependencyKind::PostprocessorVersion: return "postprocessor-version";
    case AssetDependencyKind::PrefabBase: return "prefab-base";
    case AssetDependencyKind::NestedPrefab: return "nested-prefab";
    case AssetDependencyKind::PrefabOverrideTarget: return "prefab-override-target";
    case AssetDependencyKind::RuntimeComponentCapability: return "runtime-component-capability";
    case AssetDependencyKind::RawPackageFile: return "raw-package-file";
    default: return "source-file-hash";
    }
}

nlohmann::json ToJson(const NLS::Core::Assets::ArtifactManifest& manifest)
{
    nlohmann::json root;
    root["schema"] = 1;
    root["sourceAssetId"] = manifest.sourceAssetId.ToString();
    root["importerId"] = manifest.importerId;
    root["importerVersion"] = manifest.importerVersion;
    root["targetPlatform"] = manifest.targetPlatform;
    root["primarySubAssetKey"] = manifest.primarySubAssetKey;

    root["subAssets"] = nlohmann::json::array();
    for (const auto& artifact : manifest.subAssets)
    {
        root["subAssets"].push_back({
            {"sourceAssetId", artifact.sourceAssetId.ToString()},
            {"subAssetKey", artifact.subAssetKey},
            {"artifactType", ToArtifactTypeKey(artifact.artifactType)},
            {"loaderId", artifact.loaderId},
            {"targetPlatform", artifact.targetPlatform},
            {"artifactPath", artifact.artifactPath},
            {"contentHash", artifact.contentHash}
        });
    }

    root["dependencies"] = nlohmann::json::array();
    for (const auto& dependency : manifest.dependencies)
    {
        root["dependencies"].push_back({
            {"kind", ToDependencyKindKey(dependency.kind)},
            {"value", dependency.value},
            {"hashOrVersion", dependency.hashOrVersion}
        });
    }
    return root;
}

std::optional<NLS::Core::Assets::ArtifactManifest> ManifestFromJson(const nlohmann::json& root)
{
    return ParseArtifactManifestJson(root, false);
}

std::optional<NLS::Core::Assets::ArtifactManifest> LoadManifestFile(const std::filesystem::path& manifestPath)
{
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input)
        return std::nullopt;

    const auto root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded())
        return std::nullopt;
    return ManifestFromJson(root);
}

void AddArtifactPublishRecoveryDiagnostic(
    NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const std::filesystem::path& artifactRoot,
    const std::string& message)
{
    NLS::Core::Assets::AssetDiagnostic diagnostic;
    diagnostic.severity = NLS::Core::Assets::AssetDiagnosticSeverity::Warning;
    diagnostic.code = "assetdatabase-artifact-publish-recovery-failed";
    diagnostic.path = artifactRoot;
    diagnostic.message = message;
    diagnostics.push_back(std::move(diagnostic));
}

bool HasRecoverableCommittedRollback(
    const std::filesystem::path& rollbackRoot,
    NLS::Core::Assets::AssetId sourceAssetId)
{
    const auto rollbackManifestPath = rollbackRoot / "manifest.json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(rollbackManifestPath, error))
        return false;

    auto rollbackManifest = LoadManifestFile(rollbackManifestPath);
    return rollbackManifest.has_value() && rollbackManifest->sourceAssetId == sourceAssetId;
}

bool ShouldRecoverInterruptedArtifactPublish(
    const std::filesystem::path& artifactRoot,
    NLS::Core::Assets::AssetId sourceAssetId)
{
    const auto manifestPath = artifactRoot / "manifest.json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(manifestPath, error))
        return true;

    auto manifest = LoadManifestFile(manifestPath);
    return !manifest.has_value() || manifest->sourceAssetId != sourceAssetId;
}

bool RecoverInterruptedArtifactPublish(
    const std::filesystem::path& artifactRoot,
    NLS::Core::Assets::AssetId sourceAssetId,
    NLS::Core::Assets::AssetDiagnostics& diagnostics)
{
    if (artifactRoot.empty())
        return false;

    auto rollbackRoot = artifactRoot;
    rollbackRoot += ".publish-rollback";
    if (!HasRecoverableCommittedRollback(rollbackRoot, sourceAssetId) ||
        !ShouldRecoverInterruptedArtifactPublish(artifactRoot, sourceAssetId))
    {
        return false;
    }

    std::lock_guard publishLock(GetArtifactPublishMutex(artifactRoot));
    if (!HasRecoverableCommittedRollback(rollbackRoot, sourceAssetId) ||
        !ShouldRecoverInterruptedArtifactPublish(artifactRoot, sourceAssetId))
    {
        return false;
    }

    std::error_code error;
    std::filesystem::remove_all(artifactRoot, error);
    if (error)
    {
        AddArtifactPublishRecoveryDiagnostic(
            diagnostics,
            artifactRoot,
            "Interrupted artifact publish could not remove the incomplete artifact root: " + error.message());
        return false;
    }

    error.clear();
    std::filesystem::rename(rollbackRoot, artifactRoot, error);
    if (error)
    {
        AddArtifactPublishRecoveryDiagnostic(
            diagnostics,
            artifactRoot,
            "Interrupted artifact publish could not restore the last committed artifact root: " + error.message());
        return false;
    }

    return true;
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

bool AssetDatabaseFacade::Refresh()
{
    m_diagnostics.clear();
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
    if (progressTracker)
    {
        if (!job.IsValid())
            job = progressTracker->BeginJob({}, NormalizeEditorAssetPath(assetPath), "editor", batchTotalAssets);
        progressTracker->ReportProgress(
            job,
            ImportPhase::Queued,
            0.01,
            "Preparing reimport");
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

    const auto stagingRoot = GetArtifactStagingRootForAssetPath(record->absolutePath);
    std::error_code error;
    if (!stagingRoot.empty())
        std::filesystem::remove_all(stagingRoot, error);

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

void AssetDatabaseFacade::StartAssetEditing()
{
    if (!IsEditorMode())
    {
        RejectRuntimeEditorApi("AssetDatabase.StartAssetEditing");
        return;
    }

    m_assetEditing = true;
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
            result.subAssetKey = artifact->subAssetKey;
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
        results.push_back({
            record->id,
            path,
            artifact.subAssetKey,
            artifact.artifactPath,
            artifact.artifactType,
            artifact.subAssetKey == manifest->second.primarySubAssetKey
        });
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

    SaveArtifactDatabaseManifest(manifest);
    std::lock_guard manifestLock(m_manifestMutex);
    m_manifestsBySource[manifest.sourceAssetId] = std::move(manifest);
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

    const auto meta = NLS::Core::Assets::AssetMeta::Load(record->metaPath);
    if (!meta.has_value())
        return false;

    if (manifestCopy.importerId != meta->importerId ||
        manifestCopy.importerVersion != meta->importerVersion ||
        manifestCopy.targetPlatform != "editor")
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

    return checkedAsset && checkedMeta;
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
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = subAssetKey;
    manifest.subAssets.push_back(MakeImportedArtifact(meta.id, asset, subAssetKey, absolutePath));
    AddArtifactManifest(std::move(manifest));
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
        manifest.targetPlatform = "editor";
    }

    const auto subAssetKey = MakeSubAssetKey(asset);
    if (manifest.FindSubAsset(subAssetKey))
        return false;

    if (manifest.primarySubAssetKey.empty())
        manifest.primarySubAssetKey = subAssetKey;

    manifest.subAssets.push_back(MakeImportedArtifact(record->id, asset, subAssetKey, record->absolutePath));
    m_manifestsBySource[record->id] = std::move(manifest);
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

    NLS::Core::Assets::ArtifactManifest destinationManifest;
    destinationManifest.sourceAssetId = destinationId;
    destinationManifest.importerId = meta.importerId;
    destinationManifest.targetPlatform = "editor";
    destinationManifest.primarySubAssetKey = asset.subAssetKey;
    destinationManifest.subAssets.push_back(MakeImportedArtifact(
        destinationId,
        object,
        asset.subAssetKey,
        destination));
    AddArtifactManifest(std::move(destinationManifest));
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
            "prefab.nprefab",
            std::vector<uint8_t>(sourceText.begin(), sourceText.end())
        });
        writeRequest.dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
            editorAssetPath,
            FileStamp(absolutePath)
        });
        writeRequest.dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
            ToEditorAssetPath(NLS::Core::Assets::GetAssetMetaPath(absolutePath)),
            FileStamp(NLS::Core::Assets::GetAssetMetaPath(absolutePath))
        });
        writeRequest.dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::ImporterVersion,
            meta->importerId,
            std::to_string(meta->importerVersion)
        });
        writeRequest.dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::BuildTarget,
            "editor",
            "editor"
        });

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

        const auto shaderArtifact = ImportShaderArtifactPayload(
            absolutePath,
            editorAssetPath,
            subAssetKey,
            assetRoot);

        NLS::Core::Assets::ArtifactWriteRequest writeRequest;
        writeRequest.sourceAssetId = meta->id;
        writeRequest.importerId = meta->importerId;
        writeRequest.importerVersion = meta->importerVersion;
        writeRequest.targetPlatform = "editor";
        writeRequest.primarySubAssetKey = subAssetKey;
        writeRequest.artifacts.push_back({
            subAssetKey,
            NLS::Core::Assets::ArtifactType::Shader,
            "shader",
            "shader.nshader",
            NLS::Render::Assets::SerializeShaderArtifact(shaderArtifact)
        });
        writeRequest.dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
            editorAssetPath,
            FileStamp(absolutePath)
        });
        writeRequest.dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
            ToEditorAssetPath(NLS::Core::Assets::GetAssetMetaPath(absolutePath)),
            FileStamp(NLS::Core::Assets::GetAssetMetaPath(absolutePath))
        });
        AddShaderStageSourceDependencies(
            writeRequest.dependencies,
            m_roots,
            absolutePath,
            shaderArtifact);
        writeRequest.dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::ImporterVersion,
            meta->importerId,
            std::to_string(meta->importerVersion)
        });
        writeRequest.dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::BuildTarget,
            "editor",
            "editor"
        });

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
    {
        std::lock_guard manifestLock(m_manifestMutex);
        const auto previous = m_manifestsBySource.find(record->id);
        if (previous != m_manifestsBySource.end())
            previousManifest = previous->second;

        materialShaderResourcePath = FindImportedShaderArtifactResourcePath(
            m_manifestsBySource,
            m_editorPathById,
            "Assets/Engine/Shaders/StandardPBR.hlsl");
        if (!materialShaderResourcePath.has_value())
            materialShaderResourcePath = FindImportedMaterialShaderArtifactResourcePath(m_manifestsBySource, m_editorPathById);
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
        materialShaderResourcePath.value_or(":Shaders/StandardPBR.hlsl")
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
    committedManifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
        editorAssetPath,
        FileStamp(absolutePath)
    });
    committedManifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
        ToEditorAssetPath(NLS::Core::Assets::GetAssetMetaPath(absolutePath)),
        FileStamp(NLS::Core::Assets::GetAssetMetaPath(absolutePath))
    });

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

    const auto path = std::filesystem::path(artifactPath);
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
        if (!candidate.empty() &&
            IsPathInsideEditorAssetRoot(candidate, artifactRoot) &&
            std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    for (const auto& candidate : candidates)
    {
        if (!candidate.empty() && IsPathInsideEditorAssetRoot(candidate, artifactRoot))
            return candidate;
    }

    return {};
}

std::filesystem::path AssetDatabaseFacade::GetArtifactRootForAssetPath(
    const std::filesystem::path& absolutePath) const
{
    const auto* root = FindEditorAssetRootForAbsolutePath(m_roots, absolutePath);
    if (!root)
        return {};
    const auto assetPath = ToEditorAssetPath(absolutePath);
    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record || !record->id.IsValid())
        return {};
    return GetEditorAssetRootLibraryPath(*root) / "Artifacts" / record->id.ToString();
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
    const auto artifactRoot = GetArtifactRootForAssetPath(absolutePath);
    return artifactRoot.empty() ? std::filesystem::path {} : artifactRoot / "manifest.json";
}

std::filesystem::path AssetDatabaseFacade::GetArtifactDatabasePathForAssetPath(
    const std::filesystem::path& absolutePath) const
{
    const auto* root = FindEditorAssetRootForAbsolutePath(m_roots, absolutePath);
    if (!root)
        return {};
    return GetEditorAssetRootLibraryPath(*root) / "ArtifactDB" / "index.tsv";
}

void AssetDatabaseFacade::LoadPersistedArtifactManifests()
{
    std::unordered_map<NLS::Core::Assets::AssetId, NLS::Core::Assets::ArtifactManifest> loadedManifests;
    std::vector<NLS::Core::Assets::AssetId> visitedSourceIds;
    for (const auto& record : m_sourceDatabase.GetRecords())
    {
        if (record.assetType != NLS::Core::Assets::AssetType::ModelScene &&
            record.assetType != NLS::Core::Assets::AssetType::Prefab &&
            record.assetType != NLS::Core::Assets::AssetType::Shader)
        {
            continue;
        }

        const auto artifactRoot = GetArtifactRootForAssetPath(record.absolutePath);
        RecoverInterruptedArtifactPublish(artifactRoot, record.id, m_diagnostics);
        visitedSourceIds.push_back(record.id);

        const auto manifestPath = artifactRoot.empty() ? std::filesystem::path {} : artifactRoot / "manifest.json";
        if (manifestPath.empty() || !std::filesystem::exists(manifestPath))
            continue;

        auto manifest = LoadManifestFile(manifestPath);
        if (!manifest.has_value())
        {
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Warning,
                "assetdatabase-artifact-manifest-read-failed",
                manifestPath,
                "Imported artifact manifest could not be read; reimport the source asset.");
            continue;
        }

        if (manifest->sourceAssetId != record.id)
        {
            AddDiagnostic(
                NLS::Core::Assets::AssetDiagnosticSeverity::Warning,
                "assetdatabase-artifact-manifest-source-mismatch",
                manifestPath,
                "Imported artifact manifest belongs to a different source asset id.");
            continue;
        }

        loadedManifests[manifest->sourceAssetId] = std::move(*manifest);
    }

    std::lock_guard manifestLock(m_manifestMutex);
    for (const auto& sourceId : visitedSourceIds)
        m_manifestsBySource.erase(sourceId);
    for (auto& [sourceId, manifest] : loadedManifests)
    {
        m_manifestsBySource[sourceId] = std::move(manifest);
    }
}

bool AssetDatabaseFacade::CanSaveArtifactManifestForAssetPath(const std::filesystem::path& absolutePath) const
{
    const auto manifestPath = GetArtifactManifestPathForAssetPath(absolutePath);
    if (manifestPath.empty())
        return false;

    std::error_code error;
    if (std::filesystem::exists(manifestPath, error) && std::filesystem::is_directory(manifestPath, error))
        return false;

    const auto parentPath = manifestPath.parent_path();
    if (parentPath.empty())
        return false;

    if (std::filesystem::exists(parentPath, error))
        return std::filesystem::is_directory(parentPath, error);

    return true;
}

bool AssetDatabaseFacade::SaveArtifactManifestForAssetPath(
    const std::filesystem::path& absolutePath,
    const NLS::Core::Assets::ArtifactManifest& manifest) const
{
    const auto manifestPath = GetArtifactManifestPathForAssetPath(absolutePath);
    if (manifestPath.empty())
        return false;

    std::error_code error;
    std::filesystem::create_directories(manifestPath.parent_path(), error);
    if (error)
        return false;

    auto stagingPath = manifestPath;
    stagingPath += ".staging";
    std::filesystem::remove(stagingPath, error);
    error.clear();

    std::ofstream output(stagingPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output << ToJson(manifest).dump(2);
    output.close();
    if (!output)
    {
        std::filesystem::remove(stagingPath, error);
        return false;
    }

    std::filesystem::rename(stagingPath, manifestPath, error);
    if (!error)
        return true;

    error.clear();
    std::filesystem::copy_file(stagingPath, manifestPath, std::filesystem::copy_options::overwrite_existing, error);
    if (!error)
    {
        std::filesystem::remove(stagingPath, error);
        return true;
    }

    std::filesystem::remove(stagingPath, error);
    return false;
}

void AssetDatabaseFacade::SaveArtifactDatabaseManifest(const NLS::Core::Assets::ArtifactManifest& manifest)
{
    const auto path = m_editorPathById.find(manifest.sourceAssetId);
    if (path == m_editorPathById.end())
        return;

    const auto absolutePath = ResolveAssetPath(path->second);
    if (absolutePath.empty())
        return;

    const auto databasePath = GetArtifactDatabasePathForAssetPath(absolutePath);
    if (databasePath.empty())
        return;

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
            path->second,
            NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
        m_dirtyArtifactDatabasePaths.insert(databaseKey);
        shouldSaveImmediately = !m_assetEditing;
    }

    if (shouldSaveImmediately)
        FlushArtifactDatabaseCache();
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
        "sha256:" + owner.ToString() + ":" + subAssetKey
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

    std::ofstream output(absolutePath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output << "NULLUS_NATIVE_ASSET=1\n";
    output << "NAME=" << asset.name << '\n';
    output << "SUB_ASSET_KEY=" << subAssetKey << '\n';
    output << "LOADER_ID=" << asset.loaderId << '\n';
    output << "PAYLOAD=" << asset.serializedPayload << '\n';
    return true;
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
