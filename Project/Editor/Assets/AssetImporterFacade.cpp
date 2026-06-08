#include "Assets/AssetImporterFacade.h"

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetPath.h"
#include "Assets/EditorAssetPath.h"
#include "Guid.h"
#include "Rendering/LargeSceneSettings.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <sstream>
#include <system_error>

namespace NLS::Editor::Assets
{
namespace
{
constexpr const char* kDirtySetting = "NULLUS_IMPORTER_DIRTY";
constexpr const char* kRemapPrefix = "EXTERNAL_REMAP.";
std::mutex g_reimportInProgressMutex;
std::unordered_map<std::string, size_t> g_reimportInProgressCounts;

bool IsReservedSetting(const std::string& key)
{
    return key == kDirtySetting || key.rfind(kRemapPrefix, 0u) == 0u;
}

std::string SerializeRemap(const ExternalObjectRemap& remap)
{
    return remap.sourceObjectType + "|" +
        remap.targetAssetId.ToString() + "|" +
        remap.targetSubAssetKey;
}

std::optional<ExternalObjectRemap> DeserializeRemap(
    const std::string& sourceObjectKey,
    const std::string& serialized)
{
    std::stringstream stream(serialized);
    std::string sourceObjectType;
    std::string targetAssetGuid;
    std::string targetSubAssetKey;

    if (!std::getline(stream, sourceObjectType, '|') ||
        !std::getline(stream, targetAssetGuid, '|') ||
        !std::getline(stream, targetSubAssetKey))
    {
        return std::nullopt;
    }

    const auto parsedGuid = NLS::Guid::TryParse(targetAssetGuid);
    if (!parsedGuid.has_value())
        return std::nullopt;

    return ExternalObjectRemap {
        sourceObjectKey,
        sourceObjectType,
        NLS::Core::Assets::AssetId(*parsedGuid),
        targetSubAssetKey
    };
}

std::string SerializePlatformOverride(const TexturePlatformOverride& platform)
{
    std::string serialized =
        std::to_string(platform.maxTextureSize) + "|" +
        platform.format + "|" +
        platform.compressionQuality;
    if (!platform.resizePolicy.empty() || platform.mipmapEnabled.has_value())
    {
        serialized += "|" + platform.resizePolicy + "|";
        if (platform.mipmapEnabled.has_value())
            serialized += BoolToImporterSettingString(*platform.mipmapEnabled);
    }
    return serialized;
}

std::optional<TexturePlatformOverride> DeserializePlatformOverride(
    const std::string& platformName,
    const std::string& value)
{
    std::stringstream stream(value);
    std::string maxSizeText;
    std::string format;
    std::string compressionQuality;
    std::string resizePolicy;
    std::string mipmapEnabled;
    if (!std::getline(stream, maxSizeText, '|') ||
        !std::getline(stream, format, '|') ||
        !std::getline(stream, compressionQuality, '|'))
    {
        return std::nullopt;
    }

    if (stream.good())
    {
        std::getline(stream, resizePolicy, '|');
        std::getline(stream, mipmapEnabled);
    }

    TexturePlatformOverride platform;
    platform.platform = platformName;
    platform.format = std::move(format);
    platform.compressionQuality = std::move(compressionQuality);
    platform.resizePolicy = std::move(resizePolicy);
    try
    {
        platform.maxTextureSize = static_cast<uint32_t>(std::stoul(maxSizeText));
    }
    catch (...)
    {
        return std::nullopt;
    }

    if (!mipmapEnabled.empty())
        platform.mipmapEnabled = mipmapEnabled == "true" || mipmapEnabled == "1";
    return platform;
}
}

void AssetPostprocessContext::DeclareDependency(NLS::Core::Assets::AssetDependencyRecord dependency)
{
    dependencies.push_back(std::move(dependency));
}

void AssetPostprocessContext::EmitDiagnostic(std::string code, std::string message)
{
    diagnostics.push_back({std::move(code), std::move(message)});
}

void AssetPostprocessorRegistry::Register(AssetPostprocessor postprocessor)
{
    m_postprocessors.push_back(std::move(postprocessor));
    std::sort(
        m_postprocessors.begin(),
        m_postprocessors.end(),
        [](const AssetPostprocessor& lhs, const AssetPostprocessor& rhs)
        {
            if (lhs.order != rhs.order)
                return lhs.order < rhs.order;
            return lhs.name < rhs.name;
        });
}

AssetPostprocessResult AssetPostprocessorRegistry::Run(const std::string& assetPath) const
{
    AssetPostprocessContext context;
    context.assetPath = assetPath;

    for (const auto& postprocessor : m_postprocessors)
    {
        if (postprocessor.preprocess)
            postprocessor.preprocess(context);
    }

    for (const auto& postprocessor : m_postprocessors)
    {
        if (postprocessor.postprocess)
            postprocessor.postprocess(context);
    }

    AssetPostprocessResult result;
    result.dependencies = std::move(context.dependencies);
    result.diagnostics = std::move(context.diagnostics);

    for (const auto& postprocessor : m_postprocessors)
    {
        if (!result.versionToken.empty())
            result.versionToken.push_back('|');
        result.versionToken += postprocessor.name + ":" + std::to_string(postprocessor.version);
    }

    return result;
}

AssetImporterFacade::ScopedReimportInProgress::ScopedReimportInProgress(std::string assetPath)
    : m_assetPath(AssetImporterFacade::NormalizeTrackedAssetPath(assetPath))
{
    AssetImporterFacade::RegisterReimportInProgress(m_assetPath);
}

AssetImporterFacade::ScopedReimportInProgress::~ScopedReimportInProgress()
{
    AssetImporterFacade::UnregisterReimportInProgress(m_assetPath);
}

AssetImporterFacade::AssetImporterFacade(std::vector<std::filesystem::path> roots)
    : m_roots(MakeEditorAssetRoots(roots))
{
}

AssetImporterFacade::AssetImporterFacade(std::vector<EditorAssetRoot> roots)
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

bool AssetImporterFacade::Refresh()
{
    if (m_roots.empty())
        return false;

    std::vector<NLS::Core::Assets::SourceAssetRoot> scanRoots;
    scanRoots.reserve(m_roots.size());
    for (const auto& root : m_roots)
        scanRoots.push_back({root.path, root.readOnly});

    const auto ok = m_sourceDatabase.ScanRoots(scanRoots);
    RebuildPathIndex();
    m_diagnostics.insert(
        m_diagnostics.end(),
        m_sourceDatabase.GetDiagnostics().begin(),
        m_sourceDatabase.GetDiagnostics().end());
    return ok && std::none_of(
        m_diagnostics.begin(),
        m_diagnostics.end(),
        [](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.severity == NLS::Core::Assets::AssetDiagnosticSeverity::Error;
        });
}

std::optional<ImporterRecord> AssetImporterFacade::GetAtPath(const std::string& assetPath) const
{
    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return std::nullopt;

    const auto meta = NLS::Core::Assets::AssetMeta::Load(record->metaPath);
    if (!meta.has_value() && !record->readOnly)
        return std::nullopt;

    ImporterRecord importer;
    importer.assetPath = ToEditorAssetPath(record->absolutePath);
    importer.assetId = meta.has_value() ? meta->id : record->id;
    importer.importerId = meta.has_value() ? meta->importerId : record->importerId;
    importer.importerVersion = meta.has_value() ? std::max(meta->importerVersion, record->importerVersion) : record->importerVersion;
    importer.assetType = meta.has_value() ? meta->assetType : record->assetType;

    if (!meta.has_value())
        return importer;

    importer.dirty = meta->settings.find(kDirtySetting) != meta->settings.end() &&
        meta->settings.at(kDirtySetting) == "true";

    for (const auto& [key, value] : meta->settings)
    {
        if (!IsReservedSetting(key))
            importer.serializedSettings[key] = value;
    }
    return importer;
}

bool AssetImporterFacade::SetSerializedSetting(
    const std::string& assetPath,
    std::string key,
    std::string value)
{
    if (key.empty() || IsReservedSetting(key))
        return false;

    auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return false;

    meta->settings[std::move(key)] = std::move(value);
    meta->settings[kDirtySetting] = "true";
    if (!SaveMetaForPath(assetPath, *meta))
        return false;

    const auto normalized = NormalizeEditorAssetPath(assetPath);
    if (std::find(m_dirtyAssets.begin(), m_dirtyAssets.end(), normalized) == m_dirtyAssets.end())
        m_dirtyAssets.push_back(normalized);
    return true;
}

bool AssetImporterFacade::WriteImportSettingsIfDirty(const std::string& assetPath)
{
    auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return false;

    const auto wasDirty = meta->settings.find(kDirtySetting) != meta->settings.end() &&
        meta->settings[kDirtySetting] == "true";
    if (!wasDirty)
        return false;

    meta->settings.erase(kDirtySetting);
    if (!SaveMetaForPath(assetPath, *meta))
        return false;

    const auto normalized = NormalizeEditorAssetPath(assetPath);
    m_dirtyAssets.erase(
        std::remove(m_dirtyAssets.begin(), m_dirtyAssets.end(), normalized),
        m_dirtyAssets.end());
    return true;
}

bool AssetImporterFacade::SaveAndReimport(const std::string& assetPath)
{
    return SaveAndReimport(assetPath, nullptr);
}

bool AssetImporterFacade::SaveAndReimport(
    const std::string& assetPath,
    ImportProgressTracker& progressTracker)
{
    return SaveAndReimport(assetPath, &progressTracker);
}

bool AssetImporterFacade::SaveAndReimport(
    const std::string& assetPath,
    const EditorImportBudgetRequest& budgetRequest)
{
    auto request = budgetRequest;
    if (request.assetPath.empty())
        request.assetPath = assetPath;
    else if (NormalizeEditorAssetPath(request.assetPath) != NormalizeEditorAssetPath(assetPath))
        return false;

    const auto admission = TryReserveEditorImportBudgetInternal(request, true);
    if (!admission.admitted)
        return false;

    const auto imported = SaveAndReimport(assetPath, nullptr);
    if (!imported)
        ReleaseEditorImportBudgetReservation(request);
    return imported;
}

EditorImportBudgetSnapshot AssetImporterFacade::MakeEditorImportBudget(
    const NLS::Engine::Rendering::LargeSceneSettings& settings)
{
    EditorImportBudgetSnapshot budget;
    budget.cpuBudgetUs = settings.streamingCpuBudgetUs;
    budget.ioBudgetBytes = settings.streamingIoBudgetBytes;
    budget.gpuUploadBudgetBytes = settings.streamingGpuUploadBudgetBytes;
    budget.cpuMemoryBudgetBytes = settings.streamingCpuMemoryBudgetBytes;
    budget.gpuMemoryBudgetBytes = settings.streamingGpuMemoryBudgetBytes;
    budget.remainingCpuBudgetUs = budget.cpuBudgetUs;
    budget.remainingIoBudgetBytes = budget.ioBudgetBytes;
    budget.remainingGpuUploadBudgetBytes = budget.gpuUploadBudgetBytes;
    budget.remainingCpuMemoryBudgetBytes = budget.cpuMemoryBudgetBytes;
    budget.remainingGpuMemoryBudgetBytes = budget.gpuMemoryBudgetBytes;
    return budget;
}

void AssetImporterFacade::SetEditorImportBudget(EditorImportBudgetSnapshot budget)
{
    budget.remainingCpuBudgetUs =
        budget.cpuBudgetUs > budget.reservedCpuBudgetUs
            ? budget.cpuBudgetUs - budget.reservedCpuBudgetUs
            : 0u;
    budget.remainingIoBudgetBytes =
        budget.ioBudgetBytes > budget.reservedIoBudgetBytes
            ? budget.ioBudgetBytes - budget.reservedIoBudgetBytes
            : 0u;
    budget.remainingGpuUploadBudgetBytes =
        budget.gpuUploadBudgetBytes > budget.reservedGpuUploadBudgetBytes
            ? budget.gpuUploadBudgetBytes - budget.reservedGpuUploadBudgetBytes
            : 0u;
    budget.remainingCpuMemoryBudgetBytes =
        budget.cpuMemoryBudgetBytes > budget.reservedCpuMemoryBudgetBytes
            ? budget.cpuMemoryBudgetBytes - budget.reservedCpuMemoryBudgetBytes
            : 0u;
    budget.remainingGpuMemoryBudgetBytes =
        budget.gpuMemoryBudgetBytes > budget.reservedGpuMemoryBudgetBytes
            ? budget.gpuMemoryBudgetBytes - budget.reservedGpuMemoryBudgetBytes
            : 0u;
    std::lock_guard lock(m_editorImportBudgetMutex);
    m_editorImportBudget = budget;
}

EditorImportBudgetSnapshot AssetImporterFacade::GetEditorImportBudgetSnapshot() const
{
    std::lock_guard lock(m_editorImportBudgetMutex);
    return m_editorImportBudget.value_or(EditorImportBudgetSnapshot{});
}

EditorImportBudgetAdmission AssetImporterFacade::TryReserveEditorImportBudget(
    const EditorImportBudgetRequest& request)
{
    return TryReserveEditorImportBudgetInternal(request, true);
}

bool AssetImporterFacade::SaveAndReimport(
    const std::string& assetPath,
    ImportProgressTracker* progressTracker)
{
    const auto normalized = NormalizeEditorAssetPath(assetPath);
    const ScopedReimportInProgress reimportGuard(normalized);
    const auto resolvedAssetPath = ResolveAssetPath(normalized);
    if (resolvedAssetPath.empty() || !std::filesystem::is_regular_file(resolvedAssetPath))
        return false;

    if (std::find(m_queuedReimports.begin(), m_queuedReimports.end(), normalized) == m_queuedReimports.end())
        m_queuedReimports.push_back(normalized);
    const auto removeQueuedReimport = [&]()
    {
        m_queuedReimports.erase(
            std::remove(m_queuedReimports.begin(), m_queuedReimports.end(), normalized),
            m_queuedReimports.end());
    };

    ImportJobId job;
    if (progressTracker)
    {
        job = progressTracker->BeginJob({}, normalized, "editor", 1u);
        progressTracker->ReportProgress(job, ImportPhase::Queued, 0.01, "Preparing reimport");
    }

    if (!Refresh())
    {
        removeQueuedReimport();
        if (progressTracker && job.IsValid())
            progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
        return false;
    }

    auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
    {
        removeQueuedReimport();
        if (progressTracker && job.IsValid())
            progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
        return false;
    }

    auto cleanMeta = *meta;
    cleanMeta.settings.erase(kDirtySetting);
    if (!SaveMetaForPath(assetPath, cleanMeta))
    {
        removeQueuedReimport();
        if (progressTracker && job.IsValid())
            progressTracker->FinishJob(job, ImportJobTerminalStatus::Failed, m_diagnostics);
        return false;
    }

    AssetDatabaseFacade database(m_roots);
    const auto imported = progressTracker
        ? database.ReimportAsset(normalized, *progressTracker, job)
        : database.ReimportAsset(normalized);
    if (!imported)
    {
        removeQueuedReimport();
        SaveMetaForPath(assetPath, *meta);
        return false;
    }

    m_dirtyAssets.erase(
        std::remove(m_dirtyAssets.begin(), m_dirtyAssets.end(), normalized),
        m_dirtyAssets.end());
    return true;
}

EditorImportBudgetAdmission AssetImporterFacade::TryReserveEditorImportBudgetInternal(
    const EditorImportBudgetRequest& request,
    const bool commitReservation)
{
    std::lock_guard lock(m_editorImportBudgetMutex);
    if (!m_editorImportBudget.has_value())
        return {true, "unbudgeted", {}};

    auto budget = *m_editorImportBudget;
    auto reject = [&](std::string reason)
    {
        return EditorImportBudgetAdmission{false, std::move(reason), budget};
    };

    if (request.cpuCostUs > budget.remainingCpuBudgetUs)
        return reject("cpu-budget-exhausted");
    if (request.ioBytes > budget.remainingIoBudgetBytes)
        return reject("io-budget-exhausted");
    if (request.gpuUploadBytes > budget.remainingGpuUploadBudgetBytes)
        return reject("gpu-upload-budget-exhausted");
    if (request.cpuMemoryBytes > budget.remainingCpuMemoryBudgetBytes)
        return reject("cpu-memory-budget-exhausted");
    if (request.gpuMemoryBytes > budget.remainingGpuMemoryBudgetBytes)
        return reject("gpu-memory-budget-exhausted");

    if (commitReservation)
    {
        budget.reservedCpuBudgetUs += request.cpuCostUs;
        budget.reservedIoBudgetBytes += request.ioBytes;
        budget.reservedGpuUploadBudgetBytes += request.gpuUploadBytes;
        budget.reservedCpuMemoryBudgetBytes += request.cpuMemoryBytes;
        budget.reservedGpuMemoryBudgetBytes += request.gpuMemoryBytes;
        budget.remainingCpuBudgetUs -= request.cpuCostUs;
        budget.remainingIoBudgetBytes -= request.ioBytes;
        budget.remainingGpuUploadBudgetBytes -= request.gpuUploadBytes;
        budget.remainingCpuMemoryBudgetBytes -= request.cpuMemoryBytes;
        budget.remainingGpuMemoryBudgetBytes -= request.gpuMemoryBytes;
        m_editorImportBudget = budget;
    }

    return {true, "admitted", budget};
}

void AssetImporterFacade::ReleaseEditorImportBudgetReservation(
    const EditorImportBudgetRequest& request)
{
    std::lock_guard lock(m_editorImportBudgetMutex);
    if (!m_editorImportBudget.has_value())
        return;

    auto& budget = *m_editorImportBudget;
    const auto releaseCpuUs = std::min(request.cpuCostUs, budget.reservedCpuBudgetUs);
    const auto releaseIoBytes = std::min(request.ioBytes, budget.reservedIoBudgetBytes);
    const auto releaseGpuUploadBytes = std::min(request.gpuUploadBytes, budget.reservedGpuUploadBudgetBytes);
    const auto releaseCpuMemoryBytes = std::min(request.cpuMemoryBytes, budget.reservedCpuMemoryBudgetBytes);
    const auto releaseGpuMemoryBytes = std::min(request.gpuMemoryBytes, budget.reservedGpuMemoryBudgetBytes);

    budget.reservedCpuBudgetUs -= releaseCpuUs;
    budget.reservedIoBudgetBytes -= releaseIoBytes;
    budget.reservedGpuUploadBudgetBytes -= releaseGpuUploadBytes;
    budget.reservedCpuMemoryBudgetBytes -= releaseCpuMemoryBytes;
    budget.reservedGpuMemoryBudgetBytes -= releaseGpuMemoryBytes;
    budget.remainingCpuBudgetUs = std::min(
        budget.cpuBudgetUs,
        budget.remainingCpuBudgetUs + releaseCpuUs);
    budget.remainingIoBudgetBytes = std::min(
        budget.ioBudgetBytes,
        budget.remainingIoBudgetBytes + releaseIoBytes);
    budget.remainingGpuUploadBudgetBytes = std::min(
        budget.gpuUploadBudgetBytes,
        budget.remainingGpuUploadBudgetBytes + releaseGpuUploadBytes);
    budget.remainingCpuMemoryBudgetBytes = std::min(
        budget.cpuMemoryBudgetBytes,
        budget.remainingCpuMemoryBudgetBytes + releaseCpuMemoryBytes);
    budget.remainingGpuMemoryBudgetBytes = std::min(
        budget.gpuMemoryBudgetBytes,
        budget.remainingGpuMemoryBudgetBytes + releaseGpuMemoryBytes);
}

bool AssetImporterFacade::AddRemap(const std::string& assetPath, ExternalObjectRemap remap)
{
    if (remap.sourceObjectKey.empty() || !remap.targetAssetId.IsValid())
        return false;

    auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return false;

    meta->settings[std::string(kRemapPrefix) + remap.sourceObjectKey] = SerializeRemap(remap);
    meta->settings[kDirtySetting] = "true";
    return SaveMetaForPath(assetPath, *meta);
}

bool AssetImporterFacade::RemoveRemap(const std::string& assetPath, const std::string& sourceObjectKey)
{
    auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return false;

    const auto erased = meta->settings.erase(std::string(kRemapPrefix) + sourceObjectKey) > 0u;
    if (!erased)
        return false;

    meta->settings[kDirtySetting] = "true";
    return SaveMetaForPath(assetPath, *meta);
}

std::vector<ExternalObjectRemap> AssetImporterFacade::GetExternalObjectMap(
    const std::string& assetPath) const
{
    std::vector<ExternalObjectRemap> remaps;
    const auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return remaps;

    for (const auto& [key, value] : meta->settings)
    {
        if (key.rfind(kRemapPrefix, 0u) != 0u)
            continue;

        auto remap = DeserializeRemap(key.substr(std::string(kRemapPrefix).size()), value);
        if (remap.has_value())
            remaps.push_back(*remap);
    }

    std::sort(
        remaps.begin(),
        remaps.end(),
        [](const ExternalObjectRemap& lhs, const ExternalObjectRemap& rhs)
        {
            return lhs.sourceObjectKey < rhs.sourceObjectKey;
        });
    return remaps;
}

bool AssetImporterFacade::SetModelImporterSettings(
    const std::string& assetPath,
    const ModelImporterSettings& settings)
{
    auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return false;

    meta->settings["MODEL_GLOBAL_SCALE"] = std::to_string(settings.globalScale);
    meta->settings["MODEL_AXIS_CONVERSION"] = settings.axisConversion;
    meta->settings["MODEL_UNIT_CONVERSION"] = settings.unitConversion;
    meta->settings["MODEL_HIERARCHY_POLICY"] = settings.hierarchyPolicy;
    meta->settings["MODEL_FBX_READER"] = FbxReaderSelectionToImporterSettingString(settings.fbxReaderSelection);
    meta->settings["MODEL_IMPORT_NORMALS"] = BoolToImporterSettingString(settings.importNormals);
    meta->settings["MODEL_IMPORT_TANGENTS"] = BoolToImporterSettingString(settings.importTangents);
    meta->settings["MODEL_IMPORT_UVS"] = BoolToImporterSettingString(settings.importUvs);
    meta->settings["MODEL_IMPORT_MATERIALS"] = BoolToImporterSettingString(settings.importMaterials);
    meta->settings["MODEL_IMPORT_SKELETON"] = BoolToImporterSettingString(settings.importSkeleton);
    meta->settings["MODEL_IMPORT_ANIMATIONS"] = BoolToImporterSettingString(settings.importAnimations);
    meta->settings["MODEL_IMPORT_MORPH_TARGETS"] = BoolToImporterSettingString(settings.importMorphTargets);
    meta->settings["MODEL_IMPORT_CAMERAS"] = BoolToImporterSettingString(settings.importCameras);
    meta->settings["MODEL_IMPORT_LIGHTS"] = BoolToImporterSettingString(settings.importLights);
    meta->settings[kDirtySetting] = "true";
    return SaveMetaForPath(assetPath, *meta);
}

std::optional<ModelImporterSettings> AssetImporterFacade::GetModelImporterSettings(
    const std::string& assetPath) const
{
    const auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return std::nullopt;

    return ModelImporterSettingsFromSerialized(meta->settings);
}

bool AssetImporterFacade::SetTextureImporterSettings(
    const std::string& assetPath,
    const TextureImporterSettings& settings)
{
    auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return false;

    meta->settings["TEXTURE_TYPE"] = settings.textureType;
    meta->settings["TEXTURE_SRGB"] = BoolToImporterSettingString(settings.srgbTexture);
    meta->settings["TEXTURE_ALPHA_IS_TRANSPARENCY"] = BoolToImporterSettingString(settings.alphaIsTransparency);
    meta->settings["TEXTURE_MIPMAP_ENABLED"] = BoolToImporterSettingString(settings.mipmapEnabled);
    meta->settings["TEXTURE_WRAP_MODE"] = settings.wrapMode;
    meta->settings["TEXTURE_FILTER_MODE"] = settings.filterMode;
    meta->settings["TEXTURE_MAX_SIZE"] = std::to_string(settings.maxTextureSize);
    meta->settings["TEXTURE_RESIZE_POLICY"] = settings.resizePolicy;
    meta->settings["TEXTURE_COMPRESSION_INTENT"] = settings.compressionIntent;
    meta->settings["TEXTURE_EXPLICIT_FORMAT"] = settings.explicitFormat;

    for (auto it = meta->settings.begin(); it != meta->settings.end();)
    {
        if (it->first.rfind("TEXTURE_PLATFORM.", 0u) == 0u)
            it = meta->settings.erase(it);
        else
            ++it;
    }

    for (const auto& platform : settings.platformOverrides)
    {
        if (!platform.platform.empty())
            meta->settings["TEXTURE_PLATFORM." + platform.platform] = SerializePlatformOverride(platform);
    }

    meta->settings[kDirtySetting] = "true";
    return SaveMetaForPath(assetPath, *meta);
}

std::optional<TextureImporterSettings> AssetImporterFacade::GetTextureImporterSettings(
    const std::string& assetPath) const
{
    const auto meta = LoadMetaForPath(assetPath);
    if (!meta.has_value())
        return std::nullopt;

    TextureImporterSettings settings;
    settings.textureType = StringFromImporterSettings(meta->settings, "TEXTURE_TYPE", settings.textureType);
    settings.srgbTexture = BoolFromImporterSettings(meta->settings, "TEXTURE_SRGB", settings.srgbTexture);
    settings.alphaIsTransparency = BoolFromImporterSettings(
        meta->settings,
        "TEXTURE_ALPHA_IS_TRANSPARENCY",
        settings.alphaIsTransparency);
    settings.mipmapEnabled = BoolFromImporterSettings(meta->settings, "TEXTURE_MIPMAP_ENABLED", settings.mipmapEnabled);
    settings.wrapMode = StringFromImporterSettings(meta->settings, "TEXTURE_WRAP_MODE", settings.wrapMode);
    settings.filterMode = StringFromImporterSettings(meta->settings, "TEXTURE_FILTER_MODE", settings.filterMode);
    settings.maxTextureSize = UIntFromImporterSettings(meta->settings, "TEXTURE_MAX_SIZE", settings.maxTextureSize);
    settings.resizePolicy = StringFromImporterSettings(meta->settings, "TEXTURE_RESIZE_POLICY", settings.resizePolicy);
    settings.compressionIntent = StringFromImporterSettings(
        meta->settings,
        "TEXTURE_COMPRESSION_INTENT",
        settings.compressionIntent);
    settings.explicitFormat = StringFromImporterSettings(
        meta->settings,
        "TEXTURE_EXPLICIT_FORMAT",
        settings.explicitFormat);

    for (const auto& [key, value] : meta->settings)
    {
        constexpr const char* prefix = "TEXTURE_PLATFORM.";
        if (key.rfind(prefix, 0u) != 0u)
            continue;
        auto platform = DeserializePlatformOverride(key.substr(std::string(prefix).size()), value);
        if (platform.has_value())
            settings.platformOverrides.push_back(*platform);
    }

    std::sort(
        settings.platformOverrides.begin(),
        settings.platformOverrides.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.platform < rhs.platform;
        });
    return settings;
}

size_t AssetImporterFacade::GetQueuedReimportCount() const
{
    return m_queuedReimports.size();
}

const NLS::Core::Assets::AssetDiagnostics& AssetImporterFacade::GetDiagnostics() const
{
    return m_diagnostics;
}

bool AssetImporterFacade::IsReimportInProgress(const std::string& assetPath)
{
    const auto normalized = NormalizeTrackedAssetPath(assetPath);
    std::lock_guard lock(g_reimportInProgressMutex);
    const auto found = g_reimportInProgressCounts.find(normalized);
    return found != g_reimportInProgressCounts.end() && found->second > 0u;
}

std::unique_ptr<AssetImporterFacade::ScopedReimportInProgress> AssetImporterFacade::MarkReimportInProgressForTesting(
    const std::string& assetPath)
{
    return std::make_unique<ScopedReimportInProgress>(assetPath);
}

std::string AssetImporterFacade::NormalizeTrackedAssetPath(const std::string& assetPath)
{
    return NormalizeEditorAssetPath(assetPath);
}

void AssetImporterFacade::RegisterReimportInProgress(const std::string& assetPath)
{
    if (assetPath.empty())
        return;

    std::lock_guard lock(g_reimportInProgressMutex);
    ++g_reimportInProgressCounts[assetPath];
}

void AssetImporterFacade::UnregisterReimportInProgress(const std::string& assetPath)
{
    if (assetPath.empty())
        return;

    std::lock_guard lock(g_reimportInProgressMutex);
    const auto found = g_reimportInProgressCounts.find(assetPath);
    if (found == g_reimportInProgressCounts.end())
        return;

    if (found->second <= 1u)
        g_reimportInProgressCounts.erase(found);
    else
        --found->second;
}

std::filesystem::path AssetImporterFacade::ResolveAssetPath(const std::string& assetPath) const
{
    return ResolveEditorAssetPath(m_roots, assetPath);
}

std::string AssetImporterFacade::ToEditorAssetPath(const std::filesystem::path& absolutePath) const
{
    return NLS::Editor::Assets::ToEditorAssetPath(m_roots, absolutePath);
}

const NLS::Core::Assets::SourceAssetRecord* AssetImporterFacade::FindRecordByEditorAssetPath(
    const std::string& assetPath) const
{
    const auto found = m_idByEditorPath.find(NormalizeEditorAssetPath(assetPath));
    if (found == m_idByEditorPath.end())
        return nullptr;
    return m_sourceDatabase.FindById(found->second);
}

bool AssetImporterFacade::IsWritableAssetRecord(const NLS::Core::Assets::SourceAssetRecord& record) const
{
    return !record.readOnly && IsEditorAssetPathWritable(m_roots, record.absolutePath);
}

std::optional<NLS::Core::Assets::AssetMeta> AssetImporterFacade::LoadMetaForPath(
    const std::string& assetPath) const
{
    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return std::nullopt;
    return NLS::Core::Assets::AssetMeta::Load(record->metaPath);
}

bool AssetImporterFacade::SaveMetaForPath(
    const std::string& assetPath,
    NLS::Core::Assets::AssetMeta meta)
{
    const auto* record = FindRecordByEditorAssetPath(assetPath);
    if (!record)
        return false;
    if (!IsWritableAssetRecord(*record))
        return false;
    return meta.Save(record->metaPath);
}

void AssetImporterFacade::RebuildPathIndex()
{
    m_idByEditorPath.clear();
    m_diagnostics.clear();
    for (const auto& record : m_sourceDatabase.GetRecords())
    {
        const auto editorAssetPath = ToEditorAssetPath(record.absolutePath);
        if (editorAssetPath.empty())
            continue;

        if (m_idByEditorPath.find(editorAssetPath) != m_idByEditorPath.end())
        {
            m_diagnostics.push_back({
                NLS::Core::Assets::AssetDiagnosticSeverity::Error,
                "assetimporter-editor-path-alias",
                {},
                record.absolutePath,
                "Multiple mounted asset roots produced the same importer asset path."
            });
            continue;
        }

        m_idByEditorPath[editorAssetPath] = record.id;
    }
}
}
