#pragma once

#include "Assets/AssetDiagnostics.h"
#include "Assets/AssetId.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/AssetMeta.h"
#include "Assets/AssetImporterSettings.h"
#include "Assets/ImportProgressTracker.h"
#include "Assets/SourceAssetDatabase.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
struct ExternalObjectRemap
{
    std::string sourceObjectKey;
    std::string sourceObjectType;
    NLS::Core::Assets::AssetId targetAssetId;
    std::string targetSubAssetKey;
};

struct ImporterRecord
{
    std::string assetPath;
    NLS::Core::Assets::AssetId assetId;
    std::string importerId;
    uint32_t importerVersion = 1u;
    NLS::Core::Assets::AssetType assetType = NLS::Core::Assets::AssetType::Unknown;
    std::map<std::string, std::string> serializedSettings;
    bool dirty = false;
};

struct AssetPostprocessDiagnostic
{
    std::string code;
    std::string message;
};

struct AssetPostprocessContext
{
    std::string assetPath;
    std::vector<NLS::Core::Assets::AssetDependencyRecord> dependencies;
    std::vector<AssetPostprocessDiagnostic> diagnostics;

    void DeclareDependency(NLS::Core::Assets::AssetDependencyRecord dependency);
    void EmitDiagnostic(std::string code, std::string message);
};

struct AssetPostprocessor
{
    std::string name;
    int order = 0;
    uint32_t version = 1u;
    std::function<void(AssetPostprocessContext&)> preprocess;
    std::function<void(AssetPostprocessContext&)> postprocess;
};

struct AssetPostprocessResult
{
    std::string versionToken;
    std::vector<NLS::Core::Assets::AssetDependencyRecord> dependencies;
    std::vector<AssetPostprocessDiagnostic> diagnostics;
};

class AssetPostprocessorRegistry
{
public:
    void Register(AssetPostprocessor postprocessor);
    AssetPostprocessResult Run(const std::string& assetPath) const;

private:
    std::vector<AssetPostprocessor> m_postprocessors;
};

class AssetImporterFacade
{
public:
    class ScopedReimportInProgress;

    explicit AssetImporterFacade(std::vector<std::filesystem::path> roots);
    explicit AssetImporterFacade(std::vector<EditorAssetRoot> roots);

    bool Refresh();
    std::optional<ImporterRecord> GetAtPath(const std::string& assetPath) const;
    bool SetSerializedSetting(
        const std::string& assetPath,
        std::string key,
        std::string value);
    bool WriteImportSettingsIfDirty(const std::string& assetPath);
    bool SaveAndReimport(const std::string& assetPath);
    bool SaveAndReimport(const std::string& assetPath, ImportProgressTracker& progressTracker);

    bool AddRemap(const std::string& assetPath, ExternalObjectRemap remap);
    bool RemoveRemap(const std::string& assetPath, const std::string& sourceObjectKey);
    std::vector<ExternalObjectRemap> GetExternalObjectMap(const std::string& assetPath) const;
    bool SetModelImporterSettings(const std::string& assetPath, const ModelImporterSettings& settings);
    std::optional<ModelImporterSettings> GetModelImporterSettings(const std::string& assetPath) const;
    bool SetTextureImporterSettings(const std::string& assetPath, const TextureImporterSettings& settings);
    std::optional<TextureImporterSettings> GetTextureImporterSettings(const std::string& assetPath) const;

    size_t GetQueuedReimportCount() const;
    const NLS::Core::Assets::AssetDiagnostics& GetDiagnostics() const;
    static bool IsReimportInProgress(const std::string& assetPath);
    static std::unique_ptr<ScopedReimportInProgress> MarkReimportInProgressForTesting(const std::string& assetPath);

private:
    static std::string NormalizeTrackedAssetPath(const std::string& assetPath);
    static void RegisterReimportInProgress(const std::string& assetPath);
    static void UnregisterReimportInProgress(const std::string& assetPath);

    std::filesystem::path ResolveAssetPath(const std::string& assetPath) const;
    std::string ToEditorAssetPath(const std::filesystem::path& absolutePath) const;
    const NLS::Core::Assets::SourceAssetRecord* FindRecordByEditorAssetPath(const std::string& assetPath) const;
    bool IsWritableAssetRecord(const NLS::Core::Assets::SourceAssetRecord& record) const;
    std::optional<NLS::Core::Assets::AssetMeta> LoadMetaForPath(const std::string& assetPath) const;
    bool SaveMetaForPath(const std::string& assetPath, NLS::Core::Assets::AssetMeta meta);
    bool SaveAndReimport(const std::string& assetPath, ImportProgressTracker* progressTracker);
    void RebuildPathIndex();

    std::vector<EditorAssetRoot> m_roots;
    NLS::Core::Assets::SourceAssetDatabase m_sourceDatabase;
    NLS::Core::Assets::AssetDiagnostics m_diagnostics;
    std::map<std::string, NLS::Core::Assets::AssetId> m_idByEditorPath;
    std::vector<std::string> m_dirtyAssets;
    std::vector<std::string> m_queuedReimports;
};

class AssetImporterFacade::ScopedReimportInProgress
{
public:
    explicit ScopedReimportInProgress(std::string assetPath);
    ~ScopedReimportInProgress();

    ScopedReimportInProgress(const ScopedReimportInProgress&) = delete;
    ScopedReimportInProgress& operator=(const ScopedReimportInProgress&) = delete;

private:
    std::string m_assetPath;
};
}
