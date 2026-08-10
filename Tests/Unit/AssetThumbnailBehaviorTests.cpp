#include <gtest/gtest.h>

#include "Assets/AssetThumbnailCache.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/AssetThumbnailService.h"
#include "Assets/AssetThumbnailRenderScheduler.h"
#include "Assets/EditorThumbnailPreviewRenderer.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PreviewRenderableSnapshot.h"
#include "Assets/ResidentPrefabPreviewRegistry.h"
#include "Assets/ThumbnailRendererRegistry.h"
#include "Assets/ThumbnailPreviewProxyPool.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Engine/Assets/PrefabAsset.h"
#include "GameObject.h"
#include "Guid.h"
#include "Image.h"
#include "Jobs/BackgroundJobQueue.h"
#include "Jobs/JobSystem.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/DriverSettings.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Tests/Unit/Support/DeterministicTextureRhiDevice.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace
{
using namespace NLS::Base::Profiling;

template<typename T>
class ScopedServiceOverride final
{
public:
    explicit ScopedServiceOverride(T& service)
    {
        m_hadPrevious = NLS::Core::ServiceLocator::Contains<T>();
        if (m_hadPrevious)
            m_previous = &NLS::Core::ServiceLocator::Get<T>();
        NLS::Core::ServiceLocator::Provide<T>(service);
    }

    ~ScopedServiceOverride()
    {
        if (m_hadPrevious && m_previous != nullptr)
            NLS::Core::ServiceLocator::Provide<T>(*m_previous);
        else
            NLS::Core::ServiceLocator::Remove<T>();
    }

    ScopedServiceOverride(const ScopedServiceOverride&) = delete;
    ScopedServiceOverride& operator=(const ScopedServiceOverride&) = delete;

private:
    bool m_hadPrevious = false;
    T* m_previous = nullptr;
};

std::filesystem::path MakeThumbnailPerformanceRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_thumbnail_performance_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets" / "Textures");
    return root;
}

std::string ResourceRootWithTrailingSeparator(std::filesystem::path path)
{
    if (path.empty())
        return {};
    auto value = path.string();
    if (!value.empty() && value.back() != '/' && value.back() != '\\')
        value += std::filesystem::path::preferred_separator;
    return value;
}

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

NLS::Render::Assets::MeshArtifactData TriangleMeshArtifact()
{
    NLS::Render::Assets::MeshArtifactData mesh;
    mesh.vertices.resize(3u);
    mesh.vertices[0].position[0] = -1.0f;
    mesh.vertices[0].position[1] = -0.75f;
    mesh.vertices[1].position[0] = 1.0f;
    mesh.vertices[1].position[1] = -0.75f;
    mesh.vertices[2].position[1] = 0.75f;
    mesh.indices = {0u, 1u, 2u};
    mesh.hasBoundingSphere = true;
    mesh.boundingSphere.position = NLS::Maths::Vector3(0.0f, 0.0f, 0.0f);
    mesh.boundingSphere.radius = 1.25f;
    return mesh;
}

NLS::Render::Assets::MeshArtifactData LitTriangleMeshArtifact()
{
    auto mesh = TriangleMeshArtifact();
    for (auto& vertex : mesh.vertices)
    {
        vertex.texCoords[0] = 0.5f;
        vertex.texCoords[1] = 0.5f;
        vertex.normals[0] = 0.0f;
        vertex.normals[1] = 0.0f;
        vertex.normals[2] = 1.0f;
        vertex.tangent[0] = 1.0f;
        vertex.tangent[1] = 0.0f;
        vertex.tangent[2] = 0.0f;
        vertex.bitangent[0] = 0.0f;
        vertex.bitangent[1] = 1.0f;
        vertex.bitangent[2] = 0.0f;
    }
    mesh.vertices[0].texCoords[0] = 0.0f;
    mesh.vertices[0].texCoords[1] = 1.0f;
    mesh.vertices[1].texCoords[0] = 1.0f;
    mesh.vertices[1].texCoords[1] = 1.0f;
    mesh.vertices[2].texCoords[0] = 0.5f;
    mesh.vertices[2].texCoords[1] = 0.0f;
    return mesh;
}

NLS::Render::Assets::TextureArtifactData OnePixelTextureArtifact()
{
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 1u;
    artifact.height = 1u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;

    NLS::Render::Assets::TextureArtifactMip mip;
    mip.level = 0u;
    mip.width = 1u;
    mip.height = 1u;
    mip.rowPitch = 4u;
    mip.slicePitch = 4u;
    mip.pixels = {255u, 128u, 64u, 255u};
    artifact.mips.push_back(std::move(mip));
    return artifact;
}

NLS::Render::Context::Driver& EnsureThumbnailPerformanceTestDriver()
{
    static auto driver = std::make_unique<NLS::Render::Context::Driver>([]()
    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        return settings;
    }());
    NLS::Core::ServiceLocator::Provide(*driver);
    return *driver;
}

struct DeterministicThumbnailGpuTestContext
{
    NLS::Render::Context::Driver& driver;
    std::shared_ptr<NLS::Tests::DeterministicTextureRhiDevice> device;
};

DeterministicThumbnailGpuTestContext EnsureDeterministicThumbnailGpuTestDriver()
{
    static auto driver = std::make_unique<NLS::Render::Context::Driver>([]()
    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.enableThreadedRendering = false;
        return settings;
    }());
    static auto device = std::make_shared<NLS::Tests::DeterministicTextureRhiDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(*driver, device);
    NLS::Core::ServiceLocator::Provide(*driver);
    return { *driver, device };
}

NLS::Render::Context::Driver& EnsureThumbnailPerformanceGpuTestDriver()
{
    static auto driver = std::make_unique<NLS::Render::Context::Driver>([]()
    {
        NLS::Render::Settings::DriverSettings settings;
#if defined(_WIN32)
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
#elif defined(__APPLE__)
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::METAL;
#elif defined(__linux__)
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::VULKAN;
#endif
        settings.enableExplicitRHI = true;
        settings.enableThreadedRendering = false;
        settings.enableLightGrid = true;
        return settings;
    }());
    NLS::Core::ServiceLocator::Provide(*driver);
    return *driver;
}

class ScopedThumbnailResourceManagerAssetPaths final
{
public:
    ScopedThumbnailResourceManagerAssetPaths(
        const std::filesystem::path& projectAssetsRoot,
        const std::filesystem::path& engineAssetsRoot)
    {
        const auto projectAssetsPath = ResourceRootWithTrailingSeparator(projectAssetsRoot);
        const auto engineAssetsPath = ResourceRootWithTrailingSeparator(engineAssetsRoot);
        NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
            projectAssetsPath,
            engineAssetsPath);
        NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(
            projectAssetsPath,
            engineAssetsPath);
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(
            projectAssetsPath,
            engineAssetsPath);
        NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
            projectAssetsPath,
            engineAssetsPath);
    }

    ~ScopedThumbnailResourceManagerAssetPaths()
    {
        NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths({}, {});
        NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths({}, {});
        NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    }

    ScopedThumbnailResourceManagerAssetPaths(const ScopedThumbnailResourceManagerAssetPaths&) = delete;
    ScopedThumbnailResourceManagerAssetPaths& operator=(const ScopedThumbnailResourceManagerAssetPaths&) = delete;
};

class ScopedThumbnailPerformanceJobSystem final
{
public:
    explicit ScopedThumbnailPerformanceJobSystem(
        const uint32_t backgroundWorkerCount = 1u,
        const uint32_t foregroundWorkerCount = 0u)
    {
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif

        NLS::Base::Jobs::JobSystemConfig config;
        config.workerCount = foregroundWorkerCount;
        config.backgroundWorkerCount = backgroundWorkerCount;
        m_initialized = NLS::Base::Jobs::InitializeJobSystem(config);
    }

    ~ScopedThumbnailPerformanceJobSystem()
    {
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
    }

    [[nodiscard]] bool IsInitialized() const { return m_initialized; }

private:
    bool m_initialized = false;
};

struct ThumbnailBackgroundBlocker
{
    std::atomic<bool>* started = nullptr;
    std::atomic<bool>* release = nullptr;
};

void RunThumbnailBackgroundBlocker(void* userData)
{
    auto* blocker = static_cast<ThumbnailBackgroundBlocker*>(userData);
    blocker->started->store(true, std::memory_order_release);
    while (!blocker->release->load(std::memory_order_acquire))
        std::this_thread::yield();
}

struct ReleaseThumbnailBackgroundBlockerOnExit
{
    std::atomic<bool>& release;

    ~ReleaseThumbnailBackgroundBlockerOnExit()
    {
        release.store(true, std::memory_order_release);
    }
};

void ResetThumbnailPerformanceJobSystem()
{
    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
}

size_t CountArtifactTelemetryStageForPathSuffix(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage,
    const std::string& pathSuffix)
{
    return static_cast<size_t>(std::count_if(
        records.begin(),
        records.end(),
        [stage, &pathSuffix](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == stage &&
                record.path.size() >= pathSuffix.size() &&
                record.path.compare(record.path.size() - pathSuffix.size(), pathSuffix.size(), pathSuffix) == 0;
        }));
}

bool ContainsPathWithSuffix(const std::vector<std::string>& paths, const std::string& pathSuffix)
{
    return std::any_of(
        paths.begin(),
        paths.end(),
        [&pathSuffix](const std::string& path)
        {
            return path.size() >= pathSuffix.size() &&
                path.compare(path.size() - pathSuffix.size(), pathSuffix.size(), pathSuffix) == 0;
        });
}

void ExpectResourcesPendingDiagnostic(const std::string& diagnostic)
{
    constexpr std::string_view kExpected = "thumbnail-gpu-preview-resources-pending";
    EXPECT_TRUE(
        diagnostic == kExpected ||
        (diagnostic.size() > kExpected.size() &&
            diagnostic.compare(0u, kExpected.size(), kExpected) == 0 &&
            diagnostic[kExpected.size()] == '|'))
        << diagnostic;
}

std::string ThumbnailPerformanceLibraryArtifactPath(const std::string& hash)
{
    return (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(hash)).generic_string();
}

void WriteThumbnailPerformanceArtifactDatabaseForSource(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::string& sourceAssetPath);

void WriteThumbnailPerformanceArtifactDatabase(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    WriteThumbnailPerformanceArtifactDatabaseForSource(
        root,
        manifest,
        (std::filesystem::path("Assets") / "Models" / "Hero.fbx").generic_string());
}

void WriteThumbnailPerformanceArtifactDatabaseForSource(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::string& sourceAssetPath)
{
    NLS::Core::Assets::ArtifactDatabase database;
    const auto databasePath = root / "Library" / "ArtifactDB";
    if (std::filesystem::exists(databasePath))
        (void)database.Load(databasePath);

    database.UpsertManifest(
        manifest,
        sourceAssetPath,
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(databasePath));
}

NLS::Core::Assets::ImportedArtifact MakeThumbnailPerformanceImportedArtifact(
    const NLS::Core::Assets::AssetId& sourceAssetId,
    std::string subAssetKey,
    NLS::Core::Assets::ArtifactType artifactType,
    std::string loaderId,
    std::string artifactPath,
    std::string contentHash)
{
    NLS::Core::Assets::ImportedArtifact artifact;
    artifact.sourceAssetId = sourceAssetId;
    artifact.subAssetKey = std::move(subAssetKey);
    artifact.artifactType = artifactType;
    artifact.loaderId = std::move(loaderId);
    artifact.targetPlatform = "editor";
    artifact.artifactPath = std::move(artifactPath);
    artifact.contentHash = std::move(contentHash);
    return artifact;
}

void WriteThumbnailPerformanceAsyncMaterialShader(const std::filesystem::path& root)
{
    using namespace NLS::Core::Assets;

    const auto sourcePath = (std::filesystem::path("Assets") / "Shaders" / "AsyncMaterial.shader").generic_string();
    const std::string artifactHash =
        "f001000000000000000000000000000000000000000000000000000000000001";
    const auto artifactPath = ThumbnailPerformanceLibraryArtifactPath(artifactHash);

    WriteBinaryFile(root / sourcePath, std::vector<uint8_t>{'S', 'h', 'a', 'd', 'e', 'r'});

    NLS::Render::Assets::ShaderArtifact artifact;
    artifact.sourcePath = sourcePath;
    artifact.subAssetKey = "shader:AsyncMaterial/Forward#0";
    artifact.targetPlatform = "editor";
    artifact.shaderLabLightMode = "Forward";
    artifact.shaderLabPassState = NLS::Render::ShaderLab::ShaderLabPassState {};
    WriteBinaryFile(root / artifactPath, NLS::Render::Assets::SerializeShaderArtifact(artifact));

    ArtifactManifest manifest;
    manifest.sourceAssetId = AssetId(NLS::Guid::NewDeterministic(sourcePath));
    manifest.importerId = "ShaderLabImporter";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = artifact.subAssetKey;
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        manifest.sourceAssetId,
        artifact.subAssetKey,
        ArtifactType::Shader,
        "ShaderLoader",
        artifactPath,
        artifactHash));
    WriteThumbnailPerformanceArtifactDatabaseForSource(root, manifest, sourcePath);
}

std::vector<uint8_t> TinyPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C,
        0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00,
        0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0x4A, 0x3B,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}

NLS::Editor::Assets::AssetThumbnailRequest MakeTextureRequest(
    const std::filesystem::path& root,
    std::string freshness = "source:v1")
{
    NLS::Editor::Assets::AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("20202020-2020-4020-8020-202020202020"));
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = NLS::Editor::Assets::AssetThumbnailKind::Texture;
    request.requestedSize = 64u;
    request.settingsFingerprint = "thumbnail-performance";
    request.freshnessInputs.push_back({"source", std::move(freshness)});
    return request;
}

const PerformanceStageEntry* FindThumbnailStage(
    const PerformanceStageStatsSnapshot& snapshot,
    const std::string& stageName)
{
    for (const auto& stage : snapshot.stages)
    {
        if (stage.domain == PerformanceStageDomain::Thumbnail && stage.stageName == stageName)
            return &stage;
    }
    return nullptr;
}

void ExpectThumbnailStageHasNoMainThreadWorkIfPresent(
    const PerformanceStageStatsSnapshot& snapshot,
    const std::string& stageName)
{
    const auto* stage = FindThumbnailStage(snapshot, stageName);
    if (stage == nullptr)
        return;
    EXPECT_EQ(stage->mainThreadDuration.count(), 0)
        << stageName << " must not run on the editor main thread.";
}

std::string MakeFreshnessForIndex(const size_t index)
{
    return "source:v" + std::to_string(index);
}

NLS::Editor::Assets::AssetThumbnailRequest MakeTextureRequestForIndex(
    const std::filesystem::path& root,
    const size_t index)
{
    auto request = MakeTextureRequest(root, MakeFreshnessForIndex(index));
    request.assetId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic(
        "thumbnail-performance-texture-" + std::to_string(index)));
    request.sourceAssetPath = "Assets/Textures/Hero" + std::to_string(index) + ".png";
    return request;
}

PerformanceStageStatsSnapshot CaptureThumbnailLookup(
    NLS::Editor::Assets::AssetThumbnailService& service,
    const NLS::Editor::Assets::AssetThumbnailRequest& request,
    NLS::Editor::Assets::AssetThumbnailServiceResult* result,
    std::chrono::microseconds* scenarioElapsed)
{
    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);

    const auto scenarioBegin = std::chrono::steady_clock::now();
    *result = service.GetThumbnail(request);
    *scenarioElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - scenarioBegin);

    return stats.Snapshot();
}

class StubPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::ModelPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.rgbaPixels = {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        };
        if (request.requestedSize == 0u)
            result.rgbaPixels.clear();
        return result;
    }

    size_t renderCount = 0u;
};

class TransparentVaryingRgbPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    explicit TransparentVaryingRgbPreviewRenderer(const size_t prefabDrawItemCount = 0u)
        : m_prefabDrawItemCount(prefabDrawItemCount)
    {
    }

    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::ModelPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.rgbaPixels = {
            255u, 0u, 0u, 0u,
            0u, 255u, 0u, 0u,
            0u, 0u, 255u, 0u,
            255u, 255u, 255u, 0u
        };
        if (m_prefabDrawItemCount != 0u)
        {
            auto snapshot = std::make_shared<NLS::Editor::Assets::PreviewRenderableSnapshot>();
            snapshot->drawItems.resize(m_prefabDrawItemCount);
            snapshot->expectedDrawItemCount = m_prefabDrawItemCount;
            result.previewSnapshot = std::move(snapshot);
            result.expectedSceneDrawCount = m_prefabDrawItemCount;
        }
        return result;
    }

    size_t renderCount = 0u;

private:
    size_t m_prefabDrawItemCount = 0u;
};

class OpaqueBlackPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.rgbaPixels = {
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u
        };
        return result;
    }

    size_t renderCount = 0u;
};

class PendingThenReadyPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::ModelPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        if (renderCount == 1u)
        {
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return result;
        }
        result.completedPendingReadback = true;

        result.rgbaPixels = {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        };
        return result;
    }

    size_t renderCount = 0u;
};

class PolledReadbackPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::ModelPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewSubmitResult SubmitPreview(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++submitCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult preview;
        preview.width = 2u;
        preview.height = 2u;
        preview.diagnostic = "thumbnail-gpu-preview-readback-pending";

        NLS::Editor::Assets::EditorThumbnailPreviewReadbackTicket ticket {
            NLS::Editor::Assets::BuildThumbnailPreviewReadbackRequestKey(request),
            request.requestRevision
        };
        pendingReadbacks.push_back(ticket);
        return {std::move(preview), ticket};
    }

    bool SupportsAsynchronousReadbackPolling() const override
    {
        return true;
    }

    std::vector<NLS::Editor::Assets::EditorThumbnailPreviewCompletedReadback> PollCompletedReadbacks(
        const size_t maxCount) override
    {
        ++pollCount;
        maxPollCount = (std::max)(maxPollCount, maxCount);
        // The service polls once before the initial submit. Delay the first
        // poll that can observe the submitted ticket.
        if (delayFirstPoll && pollCount == 2u)
            return {};

        std::vector<NLS::Editor::Assets::EditorThumbnailPreviewCompletedReadback> completed;
        while (!pendingReadbacks.empty() && completed.size() < maxCount)
        {
            auto ticket = pendingReadbacks.back();
            pendingReadbacks.pop_back();

            NLS::Editor::Assets::EditorThumbnailPreviewResult preview;
            preview.width = 2u;
            preview.height = 2u;
            preview.completedPendingReadback = true;
            preview.rawVisibleDrawCount = 1u;
            preview.submittedSceneDrawCount = 1u;
            preview.rgbaPixels = {
                255u, 0u, 0u, 255u,
                0u, 255u, 0u, 255u,
                0u, 0u, 255u, 255u,
                255u, 255u, 255u, 255u
            };
            completed.push_back({std::move(ticket), std::move(preview)});
        }
        return completed;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++legacyRenderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult preview;
        preview.width = 2u;
        preview.height = 2u;
        preview.diagnostic = "legacy-render-path-used";
        return preview;
    }

    size_t submitCount = 0u;
    size_t pollCount = 0u;
    size_t maxPollCount = 0u;
    size_t legacyRenderCount = 0u;
    std::vector<NLS::Editor::Assets::EditorThumbnailPreviewReadbackTicket> pendingReadbacks;
    bool delayFirstPoll = false;
};

class OrphanTrackingPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::ModelPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewSubmitResult SubmitPreview(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        submittedRevision = request.requestRevision;
        lastTicket = {
            NLS::Editor::Assets::BuildThumbnailPreviewReadbackRequestKey(request),
            request.requestRevision
        };
        NLS::Editor::Assets::EditorThumbnailPreviewResult preview;
        preview.width = 2u;
        preview.height = 2u;
        preview.diagnostic = "thumbnail-gpu-preview-readback-pending";
        return {std::move(preview), lastTicket};
    }

    std::vector<NLS::Editor::Assets::EditorThumbnailPreviewCompletedReadback> PollCompletedReadbacks(
        size_t) override
    {
        return {};
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        NLS::Editor::Assets::EditorThumbnailPreviewResult preview;
        preview.width = 2u;
        preview.height = 2u;
        preview.completedPendingReadback = true;
        preview.rgbaPixels = {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        };
        return preview;
    }

    bool OrphanReadback(
        const NLS::Editor::Assets::EditorThumbnailPreviewReadbackTicket& ticket) override
    {
        orphanedTicket = ticket;
        return true;
    }

    uint64_t submittedRevision = 0u;
    NLS::Editor::Assets::EditorThumbnailPreviewReadbackTicket lastTicket;
    NLS::Editor::Assets::EditorThumbnailPreviewReadbackTicket orphanedTicket;
};

class RetryableFailureThenReadyPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::ModelPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        if (renderCount == 1u)
        {
            result.diagnostic = "thumbnail-gpu-preview-readback-failed:previous async readback has not been completed";
            return result;
        }

        result.rgbaPixels = {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        };
        return result;
    }

    size_t renderCount = 0u;
};

template <typename T>
std::shared_ptr<T> MakeOpaqueThumbnailGpuResource()
{
    auto* storage = new uint8_t(0u);
    return std::shared_ptr<T>(
        reinterpret_cast<T*>(storage),
        [](T* pointer)
        {
            delete reinterpret_cast<uint8_t*>(pointer);
        });
}

class DirectGpuPendingPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.persistenceDeferred = deferPersistence;
        result.diagnostic = deferPersistence
            ? "thumbnail-gpu-preview-persistence-deferred"
            : "thumbnail-gpu-preview-readback-pending";
        result.gpuTexture = {
            MakeOpaqueThumbnailGpuResource<NLS::Render::RHI::RHITexture>(),
            MakeOpaqueThumbnailGpuResource<NLS::Render::RHI::RHITextureView>(),
            std::make_shared<uint8_t>(0u),
            2u,
            2u
        };
        return result;
    }

    bool deferPersistence = false;
};

class PartialResidentGpuPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.residentPreviewPartial = forcePartial || request.residentPreviewPartial;
        if (result.residentPreviewPartial && suppressPixels)
            result.diagnostic = "thumbnail-gpu-preview-resident-partial";
        result.gpuTexture = {
            MakeOpaqueThumbnailGpuResource<NLS::Render::RHI::RHITexture>(),
            MakeOpaqueThumbnailGpuResource<NLS::Render::RHI::RHITextureView>(),
            std::make_shared<uint8_t>(0u),
            2u,
            2u
        };
        if (!suppressPixels)
        {
            result.rgbaPixels = {
                255u, 0u, 0u, 255u,
                0u, 255u, 0u, 255u,
                0u, 0u, 255u, 255u,
                255u, 255u, 255u, 255u
            };
        }
        return result;
    }

    size_t renderCount = 0u;
    bool forcePartial = false;
    bool suppressPixels = false;
};

class ResidentAssemblyPendingPreviewRenderer final :
    public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++renderCount;
        renderedSubAssetKeys.push_back(request.subAssetKey);
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.diagnostic =
            "thumbnail-gpu-preview-resources-pending:prefab-scene-assembly=1/405";
        result.resourceProgressToken = renderCount;
        return result;
    }

    size_t renderCount = 0u;
    std::vector<std::string> renderedSubAssetKeys;
};

class PrefabBudgetExceededPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.diagnostic = "thumbnail-prefab-preview-budget-exceeded";
        return result;
    }

    size_t renderCount = 0u;
};

class LargePrefabAwaitingResidentPreviewRenderer final :
    public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult PumpResources(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++pumpCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult result;
        result.supported = true;
        result.diagnostic = "thumbnail-prefab-preview-awaiting-resident-load";
        return result;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        return {};
    }

    size_t pumpCount = 0u;
    size_t renderCount = 0u;
};

class CountingMaterialManager final : public NLS::Core::ResourceManagement::MaterialManager
{
public:
    Material* PrewarmArtifactWithDependencies(const std::string& path) override
    {
        ++prewarmWithDependenciesCount;
        lastPrewarmPath = path;
        return nullptr;
    }

    Material* RequestAsyncArtifact(const std::string& path, bool cancelableInterest = false) override
    {
        ++asyncRequestCount;
        lastAsyncPath = path;
        asyncRequestPaths.push_back(path);
        (void)cancelableInterest;
        return nullptr;
    }

    Material* RequestAsyncArtifactForPreview(const std::string& path, bool cancelableInterest = false) override
    {
        return RequestAsyncArtifact(path, cancelableInterest);
    }

    std::optional<AsyncPreviewRequestResult> TryRequestAsyncArtifactForPreview(
        const std::string& path,
        bool cancelableInterest = false,
        bool waitForResourceTable = false) override
    {
        (void)waitForResourceTable;
        auto* material = RequestAsyncArtifactForPreview(path, cancelableInterest);
        return AsyncPreviewRequestResult { material, true, false };
    }

    AsyncArtifactLoadProbeResult TryProbeAsyncArtifactLoad(
        const std::string&) const override
    {
        return AsyncArtifactLoadProbeResult::Pending;
    }

    size_t prewarmWithDependenciesCount = 0u;
    size_t asyncRequestCount = 0u;
    std::string lastPrewarmPath;
    std::string lastAsyncPath;
    std::vector<std::string> asyncRequestPaths;
};

class CountingMeshManager final : public NLS::Core::ResourceManagement::MeshManager
{
public:
    Mesh* PrewarmArtifact(const std::string& path) override
    {
        ++prewarmCount;
        lastPrewarmPath = path;
        return nullptr;
    }

    Mesh* RequestAsyncArtifact(const std::string& path, bool cancelableInterest = false) override
    {
        ++asyncRequestCount;
        lastAsyncPath = path;
        asyncRequestPaths.push_back(path);
        (void)cancelableInterest;
        return nullptr;
    }

    Mesh* RequestAsyncArtifactForPreview(const std::string& path, bool cancelableInterest = false) override
    {
        return RequestAsyncArtifact(path, cancelableInterest);
    }

    size_t prewarmCount = 0u;
    size_t asyncRequestCount = 0u;
    std::string lastPrewarmPath;
    std::string lastAsyncPath;
    std::vector<std::string> asyncRequestPaths;
};

NLS::Editor::Assets::AssetThumbnailRequest MakeGpuPreviewRequest(
    const std::filesystem::path& root)
{
    NLS::Editor::Assets::AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("30303030-3030-4030-8030-303030303030"));
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.subAssetKey = "mesh:Hero";
    request.kind = NLS::Editor::Assets::AssetThumbnailKind::ModelPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "stub-preview:v1";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs.push_back({"source", "source:v1"});
    request.freshnessInputs.push_back({"artifact", "artifact:v1"});
    return request;
}

void WriteNativeArtifactTextFile(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const std::string& schemaName,
    const uint32_t schemaVersion,
    const std::string& contents)
{
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = artifactType;
    metadata.schemaName = schemaName;
    metadata.schemaVersion = schemaVersion;

    const auto payload = std::vector<uint8_t>(contents.begin(), contents.end());
    WriteBinaryFile(path, NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload));
}

NLS::Engine::Serialize::ObjectId MakeObjectId(const char* guid)
{
    return NLS::Engine::Serialize::ObjectId(NLS::Guid::Parse(guid));
}

NLS::Engine::Serialize::PropertyRecord MakePreviewTransformProperty(
    const double x,
    const double y,
    const double z,
    const double sx,
    const double sy,
    const double sz)
{
    using namespace NLS::Engine::Serialize;
    return {
        "m_transform",
        PropertyValue::Object({
            {
                "m_localPosition",
                PropertyValue::Object({
                    {"x", PropertyValue::Number(x)},
                    {"y", PropertyValue::Number(y)},
                    {"z", PropertyValue::Number(z)}
                })
            },
            {
                "m_localScale",
                PropertyValue::Object({
                    {"x", PropertyValue::Number(sx)},
                    {"y", PropertyValue::Number(sy)},
                    {"z", PropertyValue::Number(sz)}
                })
            }
        })
    };
}

NLS::Engine::Serialize::PropertyValue MakePreviewVector3Value(
    const double x,
    const double y,
    const double z)
{
    using namespace NLS::Engine::Serialize;
    return PropertyValue::Object({
        {"x", PropertyValue::Number(x)},
        {"y", PropertyValue::Number(y)},
        {"z", PropertyValue::Number(z)}
    });
}

NLS::Engine::Serialize::PropertyValue MakePreviewQuaternionValue(
    const double x,
    const double y,
    const double z,
    const double w)
{
    using namespace NLS::Engine::Serialize;
    return PropertyValue::Object({
        {"x", PropertyValue::Number(x)},
        {"y", PropertyValue::Number(y)},
        {"z", PropertyValue::Number(z)},
        {"w", PropertyValue::Number(w)}
    });
}

NLS::Engine::Assets::PrefabArtifact MakePrefabArtifactWithPreviewRendererDependencies()
{
    using namespace NLS::Engine::Serialize;

    const auto prefabId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("40404040-4040-4040-8040-404040404040"));
    const auto meshId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("50505050-5050-4050-8050-505050505050"));
    const auto materialId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("60606060-6060-4060-8060-606060606060"));
    const auto meshReferenceId = NLS::Engine::Serialize::AssetId(meshId.GetGuid());
    const auto materialReferenceId = NLS::Engine::Serialize::AssetId(materialId.GetGuid());
    const auto gameObjectId = MakeObjectId("70707070-7070-4070-8070-707070707070");
    const auto meshFilterId = MakeObjectId("80808080-8080-4080-8080-808080808080");
    const auto meshRendererId = MakeObjectId("90909090-9090-4090-8090-909090909090");

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.documentId = NLS::Guid::NewDeterministic("ThumbnailPerformance.PreviewPrefab.Document");
    document.root = gameObjectId;
    document.objects.push_back(ObjectRecord{
        gameObjectId,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
        "PreviewRoot",
        "PreviewRoot",
        ObjectRecordState::Alive,
        {
            {
                "components",
                PropertyValue::Array({
                    PropertyValue::OwnedReference(meshFilterId),
                    PropertyValue::OwnedReference(meshRendererId)
                })
            },
            MakePreviewTransformProperty(3.0, 4.0, 5.0, 2.0, 2.5, 3.0)
        },
        MakeLocalIdentifierInFile(gameObjectId)});
    document.objects.push_back(ObjectRecord{
        meshFilterId,
        NLS_TYPEOF(NLS::Engine::Components::MeshFilter).GetName(),
        "MeshFilter",
        "PreviewRoot/MeshFilter",
        ObjectRecordState::Alive,
        {
            {
                "mesh",
                PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                    meshReferenceId,
                    1,
                    "mesh:Hero"))
            }
        },
        MakeLocalIdentifierInFile(meshFilterId)});
    document.objects.push_back(ObjectRecord{
        meshRendererId,
        NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName(),
        "MeshRenderer",
        "PreviewRoot/MeshRenderer",
        ObjectRecordState::Alive,
        {
            {
                "materials",
                PropertyValue::Array({
                    PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                        materialReferenceId,
                        1,
                        "material:Hero"))
                })
            }
        },
        MakeLocalIdentifierInFile(meshRendererId)});

    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = prefabId;
    artifact.graph = std::move(document);
    artifact.resolvedAssets.push_back({
        meshId,
        "Mesh",
        "mesh:Hero",
        "Library/Artifacts/50505050-5050-4050-8050-505050505050/Hero.nmesh"
    });
    artifact.resolvedAssets.push_back({
        materialId,
        "Material",
        "material:Hero",
        "Assets/Materials/Hero.mat"
    });
    return artifact;
}
}

TEST(AssetThumbnailBehaviorTests, MeshHeaderPreviewExposesSerializedBounds)
{
    const auto root = MakeThumbnailPerformanceRoot();
    const auto artifactPath = root / "bounded-mesh.nmesh";
    auto mesh = TriangleMeshArtifact();
    mesh.materialIndex = 7u;
    WriteBinaryFile(
        artifactPath,
        NLS::Render::Assets::SerializeMeshArtifact(mesh));

    const auto header = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(artifactPath);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->vertexCount, 3u);
    EXPECT_EQ(header->indexCount, 3u);
    EXPECT_EQ(header->materialIndex, 7u);
    EXPECT_TRUE(header->hasBoundingSphere);
    EXPECT_FLOAT_EQ(header->boundingSphere.radius, 1.25f);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, TextureThumbnailQueueAndGenerationEmitDiagnosticStages)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);

    const auto request = MakeTextureRequest(root);
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    const auto snapshot = stats.Snapshot();
    ASSERT_NE(FindThumbnailStage(snapshot, "ThumbnailCacheLookup"), nullptr);
    ASSERT_NE(FindThumbnailStage(snapshot, "TotalThumbnail"), nullptr);
    ASSERT_NE(FindThumbnailStage(snapshot, "EncodePreview"), nullptr);
    ASSERT_NE(FindThumbnailStage(snapshot, "StorePreviewCache"), nullptr);

    const auto* lookup = FindThumbnailStage(snapshot, "ThumbnailCacheLookup");
    ASSERT_NE(lookup, nullptr);
    ASSERT_TRUE(lookup->counters.contains("duplicateThumbnailRequestCount"));
    EXPECT_GE(lookup->counters.at("duplicateThumbnailRequestCount"), 1u);

    const auto* total = FindThumbnailStage(snapshot, "TotalThumbnail");
    ASSERT_NE(total, nullptr);
    ASSERT_TRUE(total->counters.contains("thumbnailsGeneratedThisFrame"));
    EXPECT_GE(total->counters.at("thumbnailsGeneratedThisFrame"), 1u);

    const auto* encode = FindThumbnailStage(snapshot, "EncodePreview");
    ASSERT_NE(encode, nullptr);
    ASSERT_TRUE(encode->counters.contains("encodedByteCount"));
    EXPECT_GT(encode->counters.at("encodedByteCount"), 0u);

    const auto* store = FindThumbnailStage(snapshot, "StorePreviewCache");
    ASSERT_NE(store, nullptr);
    ASSERT_TRUE(store->counters.contains("cacheWriteCount"));
    EXPECT_GE(store->counters.at("cacheWriteCount"), 1u);
    ASSERT_TRUE(store->counters.contains("storedByteCount"));
    EXPECT_GT(store->counters.at("storedByteCount"), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, DirectTextureSourceUsesCanonicalSourceIdentity)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Imported.png", TinyPng());

    AssetBrowserItem item;
    item.projectRelativePath = "Assets/Textures/Imported.png";
    item.sourceAssetPath = item.projectRelativePath;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Texture;
    item.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("30303030-3030-4030-8030-303030303030"));
    item.subAssetKey = "texture:main";
    item.artifactPath = "Library/Artifacts/imported.texture";

    const auto request = BuildAssetThumbnailRequestForItem(root, item, 96u);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->kind, AssetThumbnailKind::Texture);
    EXPECT_TRUE(request->subAssetKey.empty());
    EXPECT_TRUE(request->artifactPath.empty());
    EXPECT_FALSE(request->generatedSubAsset);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, DirectTextureSourceSharesPresentationIdentityWithImportedTexture)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Imported.png", TinyPng());

    AssetBrowserItem item;
    item.projectRelativePath = "Assets/Textures/Imported.png";
    item.sourceAssetPath = item.projectRelativePath;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Texture;
    item.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("31313131-3131-4131-8131-313131313131"));
    item.subAssetKey = "texture:main";
    item.artifactPath = "Library/Artifacts/imported.texture";

    const auto directRequest = BuildAssetThumbnailRequestForItem(root, item, 96u);
    ASSERT_TRUE(directRequest.has_value());
    ASSERT_TRUE(directRequest->directSourceTexture);
    ASSERT_TRUE(directRequest->presentationKey.size() > 0u);

    auto importedRequest = *directRequest;
    importedRequest.subAssetKey = item.subAssetKey;
    importedRequest.artifactPath = item.artifactPath;
    importedRequest.directSourceTexture = false;
    importedRequest.presentationKey.clear();

    EXPECT_NE(
        BuildAssetThumbnailCacheKey(*directRequest),
        BuildAssetThumbnailCacheKey(importedRequest));
    EXPECT_EQ(
        directRequest->presentationKey,
        BuildAssetThumbnailPresentationKey(importedRequest));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, DirectTextureSourceSharesIdentityBeforeManifestIsAvailable)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto sourcePath = std::string("Assets/Textures/DeferredManifest.png");
    WriteBinaryFile(root / sourcePath, TinyPng());

    AssetBrowserItem sourceItem;
    sourceItem.projectRelativePath = sourcePath;
    sourceItem.sourceAssetPath = sourcePath;
    sourceItem.kind = AssetBrowserItemKind::SourceAsset;
    sourceItem.type = AssetBrowserItemType::Texture;
    sourceItem.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("33333333-3333-4333-8333-333333333333"));

    auto importedItem = sourceItem;
    importedItem.kind = AssetBrowserItemKind::GeneratedSubAsset;
    importedItem.subAssetKey = "texture:main";
    importedItem.artifactPath = "Library/Artifacts/deferred.texture";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto sourceRequest = BuildAssetThumbnailRequestForItem(
        root,
        sourceItem,
        96u,
        context);
    const auto importedRequest = BuildAssetThumbnailRequestForItem(
        root,
        importedItem,
        96u,
        context);

    ASSERT_TRUE(sourceRequest.has_value());
    ASSERT_TRUE(importedRequest.has_value());
    ASSERT_TRUE(sourceRequest->directSourceTexture);
    ASSERT_TRUE(importedRequest->directSourceTexture);
    EXPECT_TRUE(sourceRequest->subAssetKey.empty());
    EXPECT_TRUE(importedRequest->subAssetKey.empty());
    EXPECT_EQ(
        sourceRequest->presentationKey,
        BuildAssetThumbnailPresentationKey(*importedRequest));
    EXPECT_NE(
        BuildAssetThumbnailCacheKey(*sourceRequest),
        BuildAssetThumbnailCacheKey(*importedRequest));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, DirectTextureSourceUsesSnapshotPrimaryKeyForPresentationIdentity)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto assetId = AssetId(
        NLS::Guid::Parse("32323232-3232-4232-8232-323232323232"));
    const auto sourcePath = std::string("Assets/Textures/Snapshot.png");
    WriteBinaryFile(root / sourcePath, TinyPng());
    const auto metaPath = root / (sourcePath + ".meta");
    const auto metaContents =
        "GUID=" + assetId.ToString() + "\nIMPORTER_ID=TextureImporter\nASSET_TYPE=texture\n";
    WriteBinaryFile(
        metaPath,
        std::vector<uint8_t>(metaContents.begin(), metaContents.end()));

    auto database = std::make_shared<AssetDatabaseFacade>(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database->Refresh());

    ArtifactManifest manifest;
    manifest.sourceAssetId = assetId;
    manifest.importerId = "TextureImporter";
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "texture:main";
    ImportedArtifact artifact;
    artifact.sourceAssetId = assetId;
    artifact.subAssetKey = manifest.primarySubAssetKey;
    artifact.artifactType = ArtifactType::Texture;
    artifact.artifactPath = "Library/Artifacts/snapshot.texture";
    manifest.subAssets.push_back(artifact);
    database->AddArtifactManifest(manifest);
    const auto databaseManifest = database->GetArtifactManifestForAssetPath(sourcePath);
    ASSERT_TRUE(databaseManifest.has_value());
    ASSERT_EQ(databaseManifest->primarySubAssetKey, "texture:main");

    AssetBrowserItem sourceItem;
    sourceItem.projectRelativePath = sourcePath;
    sourceItem.sourceAssetPath = sourcePath;
    sourceItem.kind = AssetBrowserItemKind::SourceAsset;
    sourceItem.type = AssetBrowserItemType::Texture;
    sourceItem.assetId = assetId;

    AssetThumbnailRequestBuildContext context;
    context.assetDatabaseSnapshot = AssetDatabaseFacade::CreateReadOnlySnapshot(*database);
    const auto directRequest = BuildAssetThumbnailRequestForItem(root, sourceItem, 96u, context);
    ASSERT_TRUE(directRequest.has_value());
    ASSERT_TRUE(directRequest->directSourceTexture);
    ASSERT_TRUE(directRequest->subAssetKey.empty());
    ASSERT_TRUE(directRequest->artifactPath.empty());
    ASSERT_FALSE(directRequest->presentationKey.empty());

    auto importedItem = sourceItem;
    importedItem.kind = AssetBrowserItemKind::GeneratedSubAsset;
    importedItem.subAssetKey = manifest.primarySubAssetKey;
    importedItem.artifactPath = artifact.artifactPath;
    importedItem.artifactType = ArtifactType::Texture;
    const auto importedRequest = BuildAssetThumbnailRequestForItem(root, importedItem, 96u);
    ASSERT_TRUE(importedRequest.has_value());

    EXPECT_EQ(
        directRequest->presentationKey,
        BuildAssetThumbnailPresentationKey(*importedRequest));
    EXPECT_NE(
        BuildAssetThumbnailCacheKey(*directRequest),
        BuildAssetThumbnailCacheKey(*importedRequest));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, VisibleDirectTextureDoesNotWaitForBusyBackgroundWorker)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    const ScopedThumbnailPerformanceJobSystem jobSystem(1u, 1u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    std::atomic<bool> blockerStarted = false;
    std::atomic<bool> releaseBlocker = false;
    ReleaseThumbnailBackgroundBlockerOnExit releaseOnExit {releaseBlocker};
    ThumbnailBackgroundBlocker blockerData {&blockerStarted, &releaseBlocker};
    NLS::Base::Jobs::BackgroundJobDesc blockerDesc;
    blockerDesc.function = RunThumbnailBackgroundBlocker;
    blockerDesc.userData = &blockerData;
    auto blocker = NLS::Base::Jobs::ScheduleBackgroundJob(blockerDesc);
    ASSERT_NE(blocker.id, 0u);
    for (int attempt = 0; attempt < 200 && !blockerStarted.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(blockerStarted.load(std::memory_order_acquire));

    auto request = MakeTextureRequest(root);
    request.priority = ThumbnailRequestPriority::Visible;
    request.directSourceTexture = true;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.StartNextThumbnailGeneration());

    std::optional<AssetThumbnailServiceResult> completed;
    for (int attempt = 0; attempt < 200 && !completed.has_value(); ++attempt)
    {
        completed = service.ConsumeCompletedThumbnail();
        if (!completed.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(completed.has_value())
        << "A visible direct-source texture must not wait behind a busy background worker.";
    if (completed.has_value())
        EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);

    releaseBlocker.store(true, std::memory_order_release);
    NLS::Base::Jobs::Complete(blocker);
    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, BackgroundGenerationSelectsNonGpuWorkBehindHeavyGpuLane)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    AssetThumbnailService service;
    for (size_t index = 0u; index < 9u; ++index)
    {
        auto request = MakeGpuPreviewRequest(root);
        request.sourceAssetPath = "Assets/Models/Heavy" + std::to_string(index) + ".fbx";
        request.subAssetKey = "mesh:Heavy" + std::to_string(index);
        request.previewRendererVersion = "heavy-lane:v" + std::to_string(index);
        request.priority = ThumbnailRequestPriority::Visible;
        ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    }

    const auto textureRequest = MakeTextureRequest(root);
    ASSERT_EQ(service.RequestAssetPreview(textureRequest).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.HasQueuedNonGpuThumbnailWork());

    // The CPU background lane must select the texture directly instead of
    // scanning eight heavy GPU requests and restoring them without progress.
    ASSERT_TRUE(service.StartNextThumbnailGeneration());
    EXPECT_EQ(service.GetThumbnailState(textureRequest), ThumbnailState::Preparing);
    EXPECT_TRUE(service.HasInFlightRequest());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, VisibleTextureGenerationHasIndependentBoundedCapacity)
{
    using namespace NLS::Editor::Assets;

    const ScopedThumbnailPerformanceJobSystem jobSystem(1u);
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    {
        AssetThumbnailService service;
        std::vector<AssetThumbnailRequest> requests;
        requests.reserve(6u);
        for (size_t index = 0u; index < 6u; ++index)
        {
            WriteBinaryFile(
                root / "Assets" / "Textures" / ("Visible" + std::to_string(index) + ".png"),
                TinyPng());
            auto request = MakeTextureRequestForIndex(root, index);
            request.sourceAssetPath =
                "Assets/Textures/Visible" + std::to_string(index) + ".png";
            request.priority = ThumbnailRequestPriority::Visible;
            requests.push_back(request);
            ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
        }

        for (size_t index = 0u; index < 4u; ++index)
            ASSERT_TRUE(service.StartNextThumbnailGeneration());

        EXPECT_EQ(service.GetThumbnailState(requests[0]), ThumbnailState::Preparing);
        EXPECT_EQ(service.GetThumbnailState(requests[3]), ThumbnailState::Preparing);
        EXPECT_FALSE(service.StartNextThumbnailGeneration());
        EXPECT_EQ(service.GetQueuedRequestCount(), 2u);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, DiskCacheHitReportsLookupHitWithoutRegeneration)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    const auto request = MakeTextureRequest(root);
    {
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
        const auto generated = service.GenerateNextThumbnail();
        ASSERT_TRUE(generated.has_value());
        ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    }

    AssetThumbnailService freshService;
    AssetThumbnailServiceResult result;
    std::chrono::microseconds elapsed{0};
    const auto snapshot = CaptureThumbnailLookup(
        freshService,
        request,
        &result,
        &elapsed);

    EXPECT_EQ(result.status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_GT(elapsed.count(), 0);
    ASSERT_EQ(FindThumbnailStage(snapshot, "TotalThumbnail"), nullptr);
    ASSERT_EQ(FindThumbnailStage(snapshot, "EncodePreview"), nullptr);
    ASSERT_EQ(FindThumbnailStage(snapshot, "StorePreviewCache"), nullptr);

    const auto* lookup = FindThumbnailStage(snapshot, "ThumbnailCacheLookup");
    ASSERT_NE(lookup, nullptr);
    ASSERT_TRUE(lookup->counters.contains("cacheHitCount"));
    EXPECT_EQ(lookup->counters.at("cacheHitCount"), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, MemoryCacheHitReportsLookupHitWithoutRegeneration)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    const auto request = MakeTextureRequest(root);
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    const auto memoryHit = service.GetThumbnail(request);
    const auto snapshot = stats.Snapshot();

    EXPECT_EQ(memoryHit.status, AssetThumbnailServiceStatus::Fresh);
    ASSERT_EQ(FindThumbnailStage(snapshot, "TotalThumbnail"), nullptr);
    ASSERT_EQ(FindThumbnailStage(snapshot, "EncodePreview"), nullptr);
    ASSERT_EQ(FindThumbnailStage(snapshot, "StorePreviewCache"), nullptr);

    const auto* lookup = FindThumbnailStage(snapshot, "ThumbnailCacheLookup");
    ASSERT_NE(lookup, nullptr);
    ASSERT_TRUE(lookup->counters.contains("cacheHitCount"));
    EXPECT_EQ(lookup->counters.at("cacheHitCount"), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ThumbnailGenerationBudgetTracksCpuPreparationAndGpuUploadBytes)
{
    using namespace NLS::Editor::Assets;
    using namespace NLS::Base::Profiling;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    AssetThumbnailService service;
    ThumbnailGenerationBudget budget;
    budget.previewRenderCountBudget = SIZE_MAX;
    budget.readbackCountBudget = SIZE_MAX;
    budget.cacheWriteCountBudget = 1u;
    budget.cpuPreparationByteBudget = 1024u * 1024u;
    budget.gpuUploadByteBudget = 1024u * 1024u;
    service.SetThumbnailGenerationBudget(budget);

    ASSERT_EQ(service.GetThumbnail(MakeTextureRequest(root)).status, AssetThumbnailServiceStatus::Pending);

    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        const auto generated = service.GenerateNextThumbnail();
        ASSERT_TRUE(generated.has_value());
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    }

    const auto snapshot = stats.Snapshot();
    const auto* total = FindThumbnailStage(snapshot, "TotalThumbnail");
    ASSERT_NE(total, nullptr);
    ASSERT_TRUE(total->counters.contains("cpuPreparationByteBudgetRemaining"));
    ASSERT_TRUE(total->counters.contains("gpuUploadByteBudgetRemaining"));
    EXPECT_LE(total->counters.at("cpuPreparationByteBudgetRemaining"), 1024u * 1024u);
    EXPECT_LE(total->counters.at("gpuUploadByteBudgetRemaining"), 1024u * 1024u);

    auto blockedRequest = MakeTextureRequest(root);
    blockedRequest.sourceAssetPath = "Assets/Textures/HeroBlocked.png";
    blockedRequest.freshnessInputs = {{"source", "blocked:v1"}};
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBlocked.png", TinyPng());

    ThumbnailGenerationBudget exhaustedBudget;
    exhaustedBudget.previewRenderCountBudget = SIZE_MAX;
    exhaustedBudget.readbackCountBudget = SIZE_MAX;
    exhaustedBudget.cacheWriteCountBudget = 1u;
    exhaustedBudget.cpuPreparationByteBudget = 1u;
    exhaustedBudget.gpuUploadByteBudget = 1u;
    service.SetThumbnailGenerationBudget(exhaustedBudget);
    ASSERT_EQ(service.GetThumbnail(blockedRequest).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_FALSE(service.GenerateNextThumbnail().has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, SupersededQueuedRequestsReportCancellationDiagnostics)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    constexpr size_t queuedRequestCount = 5u;
    for (size_t index = 0; index < queuedRequestCount; ++index)
        WriteBinaryFile(root / "Assets" / "Textures" / ("Hero" + std::to_string(index) + ".png"), TinyPng());

    AssetThumbnailService service;
    for (size_t index = 0; index < queuedRequestCount; ++index)
    {
        const auto request = MakeTextureRequestForIndex(root, index);
        ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    }
    ASSERT_EQ(service.GetQueuedRequestCount(), queuedRequestCount);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    service.SupersedeQueuedRequestsForGeneration("visible-range:v2");
    const auto snapshot = stats.Snapshot();

    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    const auto* total = FindThumbnailStage(snapshot, "TotalThumbnail");
    ASSERT_NE(total, nullptr);
    ASSERT_TRUE(total->counters.contains("queueBacklog"));
    EXPECT_EQ(total->counters.at("queueBacklog"), queuedRequestCount);
    ASSERT_TRUE(total->counters.contains("cancelledThumbnailRequestCount"));
    EXPECT_EQ(total->counters.at("cancelledThumbnailRequestCount"), queuedRequestCount);
    ASSERT_TRUE(total->counters.contains("cancellationLatency"));
    EXPECT_FALSE(total->counters.contains("thumbnailsGeneratedThisFrame"));
    EXPECT_EQ(FindThumbnailStage(snapshot, "EncodePreview"), nullptr);
    EXPECT_EQ(FindThumbnailStage(snapshot, "StorePreviewCache"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, InvalidCacheEntryIsTerminalInsteadOfPermanentPending)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.sourceAssetPath = "Assets/Textures/Invalid.png";
    request.kind = AssetThumbnailKind::Texture;
    request.requestedSize = 96u;
    request.previewRendererVersion = "invalid-cache-entry-test";
    request.settingsFingerprint = "invalid-cache-entry-test";
    request.freshnessInputs = { {"source", "invalid-cache-entry:v1"} };

    AssetThumbnailService service;
    const auto result = service.GetThumbnail(request);

    EXPECT_EQ(result.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(result.presentationState, ThumbnailPresentationState::Fallback);
    EXPECT_EQ(result.diagnostic, "thumbnail-cache-path-invalid");
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_FALSE(service.HasInFlightRequest());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ZeroCacheWriteBudgetDefersQueuedTextureThumbnail)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    const auto request = MakeTextureRequest(root);
    AssetThumbnailService service;
    ThumbnailGenerationBudget budget;
    budget.cacheWriteCountBudget = 0u;
    service.SetThumbnailGenerationBudget(budget);

    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    const auto generated = service.GenerateNextThumbnail();

    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto snapshot = stats.Snapshot();
    const auto* total = FindThumbnailStage(snapshot, "TotalThumbnail");
    ASSERT_NE(total, nullptr);
    ASSERT_TRUE(total->counters.contains("queueBacklog"));
    EXPECT_EQ(total->counters.at("queueBacklog"), 1u);
    ASSERT_TRUE(total->counters.contains("cacheWriteBudgetRemaining"));
    EXPECT_EQ(total->counters.at("cacheWriteBudgetRemaining"), 0u);
    ASSERT_EQ(FindThumbnailStage(snapshot, "EncodePreview"), nullptr);
    ASSERT_EQ(FindThumbnailStage(snapshot, "StorePreviewCache"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ZeroCacheWriteBudgetDoesNotStartAsyncTextureThumbnail)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    const auto request = MakeTextureRequest(root);
    AssetThumbnailService service;
    ThumbnailGenerationBudget budget;
    budget.cacheWriteCountBudget = 0u;
    service.SetThumbnailGenerationBudget(budget);

    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u);

    EXPECT_FALSE(service.StartNextThumbnailGeneration());
    EXPECT_FALSE(service.HasInFlightRequest());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_FALSE(service.ConsumeCompletedThumbnail().has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, AsyncTextureGenerationRecordsEncodeAndStoreAsBackgroundWork)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    const auto request = MakeTextureRequest(root);
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    PerformanceStageStats stats;
    std::optional<AssetThumbnailServiceResult> completed;
    {
        PerformanceStageStatsCapture capture(stats);
        ASSERT_TRUE(service.StartNextThumbnailGeneration());
        for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
        {
            completed = service.ConsumeCompletedThumbnail();
            if (!completed.has_value())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);

    const auto snapshot = stats.Snapshot();
    const auto* encode = FindThumbnailStage(snapshot, "EncodePreview");
    ASSERT_NE(encode, nullptr);
    EXPECT_GT(encode->backgroundThreadDuration.count(), 0);
    EXPECT_EQ(encode->mainThreadDuration.count(), 0);

    const auto* store = FindThumbnailStage(snapshot, "StorePreviewCache");
    ASSERT_NE(store, nullptr);
    EXPECT_GT(store->backgroundThreadDuration.count(), 0);
    EXPECT_EQ(store->mainThreadDuration.count(), 0);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPreviewCacheWriteRunsAsBackgroundWorkAfterReadback)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    StubPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    PerformanceStageStats stats;
    std::optional<AssetThumbnailServiceResult> pending;
    std::optional<AssetThumbnailServiceResult> completed;
    {
        PerformanceStageStatsCapture capture(stats);
        pending = service.GenerateNextThumbnail(renderer);
        ASSERT_TRUE(pending.has_value());
        EXPECT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
        EXPECT_EQ(pending->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
        EXPECT_TRUE(service.HasInFlightRequest());

        const auto afterSubmit = stats.Snapshot();
        ExpectThumbnailStageHasNoMainThreadWorkIfPresent(afterSubmit, "EncodePreview");
        ExpectThumbnailStageHasNoMainThreadWorkIfPresent(afterSubmit, "StorePreviewCache");

        for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
        {
            completed = service.ConsumeCompletedThumbnail();
            if (!completed.has_value())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(renderer.renderCount, 1u);
    EXPECT_FALSE(service.HasInFlightRequest());

    const auto snapshot = stats.Snapshot();
    const auto* encode = FindThumbnailStage(snapshot, "EncodePreview");
    ASSERT_NE(encode, nullptr);
    EXPECT_GT(encode->backgroundThreadDuration.count(), 0);
    EXPECT_EQ(encode->mainThreadDuration.count(), 0);

    const auto* store = FindThumbnailStage(snapshot, "StorePreviewCache");
    ASSERT_NE(store, nullptr);
    EXPECT_GT(store->backgroundThreadDuration.count(), 0);
    EXPECT_EQ(store->mainThreadDuration.count(), 0);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, PendingReadbackPublishesGpuTextureBeforePngCacheWrite)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::ModelPreview;
    request.previewRendererVersion = "direct-gpu-publication:v1";
    request.settingsFingerprint = "direct-gpu-publication";

    DirectGpuPendingPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_TRUE(generated->gpuTexture.IsValid());
    EXPECT_EQ(generated->presentationState, ThumbnailPresentationState::Ready);
    EXPECT_EQ(generated->previewQuality, ThumbnailPreviewQuality::Canonical);
    EXPECT_TRUE(generated->refreshPending);
    EXPECT_FALSE(std::filesystem::exists(generated->imagePath))
        << "Direct GPU publication must not wait for the persistence PNG.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, PartialResidentGpuPresentationIsMemoryOnlyAndRevisionInvalidates)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.subAssetKey = "prefab:Hero";
    request.previewRendererVersion = "resident-partial:v1";
    request.settingsFingerprint = "resident-partial";

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto partialSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    partialSnapshot->drawItems.resize(1u);
    partialSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot("runtime:resident-partial", "deps:v1", partialSnapshot, 1u);
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident-partial",
        "deps:v1",
        partialSnapshot,
        registry
    };

    PartialResidentGpuPreviewRenderer renderer;
    renderer.suppressPixels = true;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto partial = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(partial->diagnostic, "thumbnail-gpu-preview-resident-partial");
    EXPECT_TRUE(partial->residentPreviewPartial);
    EXPECT_EQ(partial->residentPreviewRevision, 1u);
    EXPECT_TRUE(partial->gpuTexture.IsValid());
    EXPECT_EQ(partial->presentationState, ThumbnailPresentationState::Ready);
    EXPECT_FALSE(std::filesystem::exists(partial->imagePath));
    EXPECT_EQ(renderer.renderCount, 1u);

    const auto stable = service.GetThumbnail(request);
    EXPECT_EQ(stable.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_TRUE(stable.residentPreviewPartial);
    EXPECT_TRUE(stable.gpuTexture.IsValid());
    EXPECT_EQ(renderer.renderCount, 1u)
        << "The same partial registry revision must reuse the in-memory GPU presentation.";
    EXPECT_TRUE(service.HasQueuedGpuPreviewResourceContinuation())
        << "A partial resident frame must retain a parked resource continuation.";
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u)
        << "The parked continuation must not rerender until the registry revision advances.";

    renderer.suppressPixels = false;
    auto completeSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    completeSnapshot->drawItems.resize(2u);
    completeSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot("runtime:resident-partial", "deps:v1", completeSnapshot, 2u);

    const auto invalidated = service.GetThumbnail(request);
    EXPECT_EQ(invalidated.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_FALSE(invalidated.residentPreviewPartial);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u)
        << "A completed resident snapshot must invalidate the provisional stable result and requeue generation.";

    std::optional<AssetThumbnailServiceResult> regenerated;
    for (int attempt = 0; attempt < 4 && !regenerated.has_value(); ++attempt)
        regenerated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(regenerated.has_value());
    EXPECT_FALSE(regenerated->residentPreviewPartial)
        << "A completed resident snapshot must render as canonical, not another provisional frame.";
    EXPECT_EQ(renderer.renderCount, 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, UnloadedResidentIdentityDoesNotCreatePersistentPromotionOwner)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect resident request ownership.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto registry = ResidentPrefabPreviewRegistry::Create();
    AssetThumbnailService service;

    auto parent = MakeGpuPreviewRequest(root);
    parent.kind = AssetThumbnailKind::PrefabPreview;
    parent.sourceAssetPath = "Assets/Models/Unloaded.fbx";
    parent.subAssetKey = "prefab:Unloaded";
    parent.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:unloaded-parent",
        "deps:unloaded-parent",
        {},
        registry
    };
    EXPECT_EQ(service.GetThumbnail(parent).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetResidentPreviewOwnerCountForTesting(), 0u)
        << "A parent asset with only a resident lookup identity must use the cold generation path.";

    auto child = parent;
    child.kind = AssetThumbnailKind::ModelPreview;
    child.subAssetKey = "mesh:UnloadedChild";
    child.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:unloaded-child",
        "deps:unloaded-child",
        {},
        registry
    };
    EXPECT_EQ(service.GetThumbnail(child).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetResidentPreviewOwnerCountForTesting(), 0u)
        << "Generated child assets without a snapshot must not become permanent resident scan candidates.";

    service.MaintainPendingThumbnailRequests();
    EXPECT_EQ(service.GetResidentPreviewOwnerCountForTesting(), 0u);
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, ThumbnailServiceShutdownIsIdempotentAndClearsResidentOwners)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect resident request ownership.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    snapshot->drawItems.resize(1u);
    snapshot->expectedDrawItemCount = 1u;
    registry->RegisterSnapshot("runtime:shutdown", "deps:shutdown", snapshot, 1u);

    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.subAssetKey = "prefab:Shutdown";
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:shutdown",
        "deps:shutdown",
        snapshot,
        registry
    };

    AssetThumbnailService service;
    EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetResidentPreviewOwnerCountForTesting(), 1u);

    service.Shutdown();
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_EQ(service.GetResidentPreviewOwnerCountForTesting(), 0u);
    service.Shutdown();

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, ResidentCompletionPromotesInPlacePackageWithoutRevisionChange)
{
    using namespace NLS::Editor::Assets;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.subAssetKey = "prefab:InPlace";
    request.previewRendererVersion = "resident-in-place:v1";
    request.settingsFingerprint = "resident-in-place";

    auto registry = ResidentPrefabPreviewRegistry::Create();
    auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    snapshot->drawItems.resize(1u);
    snapshot->expectedDrawItemCount = 1u;
    auto resources = std::make_shared<ResidentPrefabPreviewResources>();
    resources->sourceDrawItemCount = 1u;
    resources->sourceExpectedDrawItemCount = 1u;
    resources->drawItems.resize(1u);
    resources->hasUnresolvedTextureBindings = true;
    registry->RegisterSnapshot(
        "runtime:resident-in-place",
        "deps:v1",
        snapshot,
        1u,
        false,
        {},
        {},
        resources);
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident-in-place",
        "deps:v1",
        snapshot,
        registry
    };

    PartialResidentGpuPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto partial = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(partial.has_value());
    EXPECT_TRUE(partial->residentPreviewPartial);
    ASSERT_EQ(partial->residentPreviewRevision, 1u);

    // Exercise the failure-consumed path: the scene can finish resolving after
    // the provisional continuation has already reached its bounded timeout.
    service.SetGpuPreviewResourcePendingAgeForTesting(request, std::chrono::seconds(121));
    service.MaintainPendingThumbnailRequests();
    ASSERT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);
    const auto timeout = service.ConsumeCompletedThumbnail();
    ASSERT_TRUE(timeout.has_value());
    EXPECT_EQ(timeout->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_TRUE(timeout->diagnostic.rfind(
        "thumbnail-gpu-preview-resources-timeout:", 0u) == 0u);

    // The live package finishes its binding in place. Its identity and
    // registry revision remain stable, but the completion state changes.
    resources->hasUnresolvedTextureBindings = false;

    const auto promoted = service.GetThumbnail(request);
    EXPECT_EQ(promoted.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_FALSE(promoted.residentPreviewPartial);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_TRUE(service.HasQueuedReadyResidentThumbnail())
        << "An in-place resident completion must be visible to scheduler admission after recovery.";

    const auto regenerated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(regenerated.has_value());
    EXPECT_FALSE(regenerated->residentPreviewPartial);
    EXPECT_EQ(renderer.renderCount, 2u);

    std::optional<AssetThumbnailServiceResult> completed;
    for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
    {
        completed = service.ConsumeCompletedThumbnail();
        if (!completed.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_FALSE(completed->residentPreviewPartial);
    EXPECT_TRUE(std::filesystem::exists(completed->imagePath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ResidentResourceContinuationOutlivesVisibleDeadline)
{
    using namespace NLS::Editor::Assets;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.priority = ThumbnailRequestPriority::Visible;
    request.subAssetKey = "prefab:VisibleTimeout";
    request.previewRendererVersion = "resident-visible-timeout:v1";
    request.settingsFingerprint = "resident-visible-timeout";

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto partialSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    partialSnapshot->drawItems.resize(1u);
    partialSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot("runtime:resident-visible-timeout", "deps:v1", partialSnapshot, 1u);
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident-visible-timeout",
        "deps:v1",
        partialSnapshot,
        registry
    };

    PartialResidentGpuPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto partial = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(partial.has_value());
    ASSERT_EQ(partial->diagnostic, "thumbnail-gpu-preview-resident-partial");

    service.SetVisibleThumbnailRequestAgeForTesting(request, std::chrono::seconds(31));
    service.MaintainPendingThumbnailRequests();
    EXPECT_FALSE(service.ConsumeCompletedThumbnail(false).has_value());
    EXPECT_NE(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_TRUE(service.HasQueuedGpuPreviewResourceContinuation());
    EXPECT_FALSE(service.HasQueuedGpuPreviewSceneAssemblyContinuation())
        << "A partial resident package is waiting for scene resources, not holding the renderer's scene-assembly cursor.";

    auto completeSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    completeSnapshot->drawItems.resize(2u);
    completeSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:resident-visible-timeout",
        "deps:v1",
        completeSnapshot,
        2u);

    const auto promoted = service.GetThumbnail(request);
    EXPECT_EQ(promoted.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_FALSE(promoted.residentPreviewPartial);
    EXPECT_TRUE(service.HasQueuedReadyResidentThumbnail());

    const auto regenerated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(regenerated.has_value());
    EXPECT_FALSE(regenerated->residentPreviewPartial);
    EXPECT_EQ(renderer.renderCount, 2u);

    std::optional<AssetThumbnailServiceResult> completed;
    for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
    {
        completed = service.ConsumeCompletedThumbnail();
        if (!completed.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_FALSE(completed->residentPreviewPartial);
    EXPECT_TRUE(std::filesystem::exists(completed->imagePath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, LightGpuLaneDoesNotConsumeCompletedResidentPromotion)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.subAssetKey = "prefab:LightLanePromotion";
    request.previewRendererVersion = "resident-light-lane:v1";
    request.settingsFingerprint = "resident-light-lane";

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto partialSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    partialSnapshot->drawItems.resize(1u);
    partialSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot("runtime:resident-light-lane", "deps:v1", partialSnapshot, 1u);
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident-light-lane",
        "deps:v1",
        partialSnapshot,
        registry
    };

    PartialResidentGpuPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());
    ASSERT_EQ(renderer.renderCount, 1u);

    auto completeSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    completeSnapshot->drawItems.resize(2u);
    completeSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:resident-light-lane",
        "deps:v1",
        completeSnapshot,
        2u);

    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.HasQueuedReadyResidentThumbnail());

    // The light pump must not consume the one-shot promotion marker. Doing so
    // would remove the only signal that bypasses the heavy-preview cooldown.
    EXPECT_FALSE(service.GenerateNextThumbnail(renderer, false).has_value());
    EXPECT_EQ(renderer.renderCount, 1u);
    EXPECT_TRUE(service.HasQueuedReadyResidentThumbnail());

    const auto canonical = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(canonical.has_value());
    EXPECT_EQ(renderer.renderCount, 2u);
    EXPECT_FALSE(canonical->residentPreviewPartial);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, CompleteResidentRetryKeepsPromotionAfterTransientPartialResult)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.subAssetKey = "prefab:ResidentTransientPartial";
    request.previewRendererVersion = "resident-transient-partial:v1";
    request.settingsFingerprint = "resident-transient-partial";

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto partialSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    partialSnapshot->drawItems.resize(1u);
    partialSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:resident-transient-partial",
        "deps:v1",
        partialSnapshot,
        1u);
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident-transient-partial",
        "deps:v1",
        partialSnapshot,
        registry
    };

    PartialResidentGpuPreviewRenderer renderer;
    renderer.forcePartial = true;
    renderer.suppressPixels = true;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto initial = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(initial.has_value());
    EXPECT_EQ(initial->diagnostic, "thumbnail-gpu-preview-resident-partial");

    auto completeSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    completeSnapshot->drawItems.resize(2u);
    completeSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:resident-transient-partial",
        "deps:v1",
        completeSnapshot,
        2u);

    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.HasQueuedReadyResidentThumbnail());

    const auto transient = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(transient.has_value());
    EXPECT_EQ(transient->diagnostic, "thumbnail-gpu-preview-resident-partial");
    EXPECT_TRUE(service.HasQueuedReadyResidentThumbnail())
        << "A complete resident package must retain promotion after a transient partial renderer result.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, CompleteResidentAssemblyYieldsAfterOnePromotion)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.subAssetKey = "prefab:ResidentAssemblyPending";
    request.previewRendererVersion = "resident-assembly-pending:v1";
    request.settingsFingerprint = "resident-assembly-pending";

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto completeSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    completeSnapshot->drawItems.resize(405u);
    completeSnapshot->expectedDrawItemCount = 405u;
    registry->RegisterSnapshot(
        "runtime:resident-assembly-pending",
        "deps:v1",
        completeSnapshot,
        405u);
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident-assembly-pending",
        "deps:v1",
        completeSnapshot,
        registry
    };

    ResidentAssemblyPendingPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.HasQueuedReadyResidentThumbnail());

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(
        pending->diagnostic,
        "thumbnail-gpu-preview-resources-pending:prefab-scene-assembly=1/405");
    EXPECT_EQ(renderer.renderCount, 1u);
    EXPECT_FALSE(service.HasQueuedReadyResidentThumbnail())
        << "The same complete registry revision must not regain one-shot priority "
           "while its scene assembly is still pending.";
    EXPECT_TRUE(service.HasQueuedGpuPreviewResourceContinuation());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, PrefabSceneAssemblyContinuationDoesNotInterleaveRequests)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto firstRequest = MakeGpuPreviewRequest(root);
    firstRequest.kind = AssetThumbnailKind::PrefabPreview;
    firstRequest.subAssetKey = "prefab:AssemblyFirst";
    firstRequest.previewRendererVersion = "assembly-continuation:v1";
    firstRequest.settingsFingerprint = "assembly-continuation";

    const auto firstRegistry = ResidentPrefabPreviewRegistry::Create();
    auto firstSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    firstSnapshot->drawItems.resize(405u);
    firstSnapshot->expectedDrawItemCount = 405u;
    firstRegistry->RegisterSnapshot(
        "runtime:assembly-first",
        "deps:v1",
        firstSnapshot,
        405u);
    firstRequest.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:assembly-first",
        "deps:v1",
        firstSnapshot,
        firstRegistry
    };

    ResidentAssemblyPendingPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(firstRequest).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());
    ASSERT_EQ(renderer.renderedSubAssetKeys.size(), 1u);
    EXPECT_EQ(renderer.renderedSubAssetKeys.front(), firstRequest.subAssetKey);

    auto secondRequest = firstRequest;
    secondRequest.subAssetKey = "prefab:AssemblySecond";
    const auto secondRegistry = ResidentPrefabPreviewRegistry::Create();
    auto secondSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    secondSnapshot->drawItems.resize(32u);
    secondSnapshot->expectedDrawItemCount = 32u;
    secondRegistry->RegisterSnapshot(
        "runtime:assembly-second",
        "deps:v1",
        secondSnapshot,
        32u);
    secondRequest.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:assembly-second",
        "deps:v1",
        secondSnapshot,
        secondRegistry
    };

    ASSERT_EQ(service.GetThumbnail(secondRequest).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.HasQueuedReadyResidentThumbnail());
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());
    ASSERT_EQ(renderer.renderedSubAssetKeys.size(), 2u);
    EXPECT_EQ(renderer.renderedSubAssetKeys[1], firstRequest.subAssetKey)
        << "Switching requests during time-sliced scene assembly releases the shared preview "
           "proxies and restarts both assembly cursors.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, CompleteResidentTakeoverReplacesReadyPartialPresentation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.subAssetKey = "prefab:ResidentReadyTakeover";
    request.previewRendererVersion = "resident-ready-takeover:v1";
    request.settingsFingerprint = "resident-ready-takeover";

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto partialSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    partialSnapshot->drawItems.resize(1u);
    partialSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:resident-ready-takeover",
        "deps:v1",
        partialSnapshot,
        1u);
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident-ready-takeover",
        "deps:v1",
        partialSnapshot,
        registry
    };

    PartialResidentGpuPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto partial = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(partial.has_value());
    ASSERT_TRUE(partial->residentPreviewPartial);

    // Simulate a queue-owner rotation after the partial frame was published.
    // The stable GPU result remains visible after a queue-owner rotation. Set
    // the state to Ready to model the lookup/maintenance interleaving that can
    // otherwise leave a provisional frame behind.
    service.DropGpuPreviewResourcePendingOwnershipForTesting(request);
    const auto readyPartial = service.GetThumbnail(request);
    EXPECT_EQ(readyPartial.status, AssetThumbnailServiceStatus::Pending);
    service.SetThumbnailStateForTesting(request, ThumbnailState::Ready);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Ready);

    auto completeSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    completeSnapshot->drawItems.resize(2u);
    completeSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:resident-ready-takeover",
        "deps:v1",
        completeSnapshot,
        2u);

    const auto promoted = service.GetThumbnail(request);
    EXPECT_EQ(promoted.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_FALSE(promoted.residentPreviewPartial);
    EXPECT_TRUE(service.HasQueuedReadyResidentThumbnail());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto regenerated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(regenerated.has_value());
    EXPECT_FALSE(regenerated->residentPreviewPartial);
    EXPECT_EQ(renderer.renderCount, 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ResidentResourceCompletionUsesReadyDrawItems)
{
    using namespace NLS::Editor::Assets;

    ResidentPrefabPreviewResources resources;
    resources.sourceDrawItemCount = 405u;
    resources.sourceExpectedDrawItemCount = 405u;
    resources.drawItems.resize(35u);
    EXPECT_FALSE(resources.IsCompleteForSource())
        << "A resident package with only the ready subset must stay provisional.";

    resources.drawItems.resize(405u);
    EXPECT_TRUE(resources.IsCompleteForSource())
        << "The package becomes complete only after every source draw item is ready.";

    ResidentPrefabPreviewResources legacyResources;
    legacyResources.drawItems.resize(1u);
    EXPECT_TRUE(legacyResources.IsCompleteForSource())
        << "Legacy fixtures without source topology metadata remain complete.";
}

TEST(AssetThumbnailBehaviorTests, ResidentResourceMissingTexturesRemainsProvisional)
{
    using namespace NLS::Editor::Assets;

    ResidentPrefabPreviewResources resources;
    resources.drawItems.resize(1u);
    resources.sourceDrawItemCount = 1u;
    resources.sourceExpectedDrawItemCount = 1u;
    resources.hasUnresolvedTextureBindings = true;

    EXPECT_FALSE(resources.IsCompleteForSource())
        << "A ready mesh/material package with missing texture uploads must stay in-memory only.";

    resources.hasUnresolvedTextureBindings = false;
    EXPECT_TRUE(resources.IsCompleteForSource())
        << "The same package becomes durable once all texture bindings are resident.";
}

TEST(AssetThumbnailBehaviorTests, ResidentResourceMissingMaterialsRemainsProvisional)
{
    using namespace NLS::Editor::Assets;

    ResidentPrefabPreviewResources resources;
    resources.drawItems.resize(1u);
    resources.sourceDrawItemCount = 1u;
    resources.sourceExpectedDrawItemCount = 1u;
    resources.hasUnresolvedMaterialBindings = true;

    EXPECT_FALSE(resources.IsCompleteForSource())
        << "A ready geometry package with missing material bindings must stay in-memory only.";

    resources.hasUnresolvedMaterialBindings = false;
    EXPECT_TRUE(resources.IsCompleteForSource())
        << "The same package becomes durable once material bindings are resident.";
}

TEST(AssetThumbnailBehaviorTests, ResidentSnapshotStateRequiresCompleteResources)
{
    using namespace NLS::Editor::Assets;

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    snapshot->drawItems.resize(2u);
    snapshot->expectedDrawItemCount = 2u;

    auto resources = std::make_shared<ResidentPrefabPreviewResources>();
    resources->drawItems.resize(2u);
    resources->sourceDrawItemCount = 2u;
    resources->sourceExpectedDrawItemCount = 2u;
    resources->hasUnresolvedTextureBindings = true;
    registry->RegisterSnapshot(
        "runtime:resident-resource-state",
        "deps:v1",
        snapshot,
        2u,
        false,
        {},
        {},
        resources);

    const auto provisional = registry->GetSnapshotState(
        "runtime:resident-resource-state",
        "deps:v1");
    ASSERT_TRUE(provisional.has_value());
    EXPECT_EQ(provisional->readyDrawItemCount, 2u);
    EXPECT_EQ(provisional->expectedDrawItemCount, 2u);
    EXPECT_FALSE(provisional->complete)
        << "Full topology must remain provisional while texture bindings are unresolved.";

    resources->hasUnresolvedTextureBindings = false;
    registry->RegisterSnapshot(
        "runtime:resident-resource-state",
        "deps:v1",
        snapshot,
        2u,
        false,
        {},
        {},
        resources);

    const auto complete = registry->GetSnapshotState(
        "runtime:resident-resource-state",
        "deps:v1");
    ASSERT_TRUE(complete.has_value());
    EXPECT_TRUE(complete->complete)
        << "The same topology becomes complete after all texture bindings are resident.";
}

TEST(AssetThumbnailBehaviorTests, ImportedModelGpuBudgetFailureStaysGpuPendingWithoutCpuRasterFallback)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto artifactPath = ThumbnailPerformanceLibraryArtifactPath(
        "d101000000000000000000000000000000000000000000000000000000000001");
    WriteBinaryFile(
        root / "Assets" / "Models" / "Hero.fbx",
        std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        root / artifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        "imported-model-prefab");

    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.subAssetKey = "prefab:Hero";
    request.artifactPath = artifactPath;
    request.previewRendererVersion = "prefab-budget-gpu-pending:v1";
    request.settingsFingerprint = "prefab-budget-gpu-pending";

    PrefabBudgetExceededPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto gpuResult = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(gpuResult.has_value());
    EXPECT_EQ(gpuResult->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(gpuResult->diagnostic, "thumbnail-gpu-preview-complexity-pending");
    EXPECT_EQ(renderer.renderCount, 1u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    // Simulate the queue/generation bookkeeping race that can leave the
    // state and queued request intact while the auxiliary ownership table is
    // briefly absent. WaitingForResources must still be scheduled as a
    // continuation and the next pump must repair ownership.
    service.DropGpuPreviewResourcePendingOwnershipForTesting(request);
    EXPECT_TRUE(service.HasQueuedGpuPreviewResourceContinuation());
    const auto repaired = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(repaired.has_value());
    EXPECT_EQ(repaired->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(repaired->diagnostic, "thumbnail-gpu-preview-complexity-pending");
    EXPECT_TRUE(service.HasQueuedGpuPreviewResourceContinuation());

    // The resolved artifact request must remain a continuation owner even if
    // both the auxiliary pending table and ordinary lane entry are lost during
    // a dequeue/validation transition.
    service.DropGpuPreviewResourcePendingOwnershipForTesting(request);
    service.DropGpuPreviewResourceQueueOwnershipForTesting(request);
    EXPECT_TRUE(service.HasQueuedGpuPreviewResourceContinuation());
    const auto restored = service.GetThumbnail(request);
    EXPECT_EQ(restored.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_TRUE(service.HasQueuedGpuPreviewResourceContinuation());

    EXPECT_FALSE(service.StartNextThumbnailGeneration())
        << "GPU-capable model and prefab previews must not enter the CPU raster worker path.";
    EXPECT_FALSE(std::filesystem::exists(gpuResult->imagePath));
    EXPECT_NE(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, LargeColdPrefabStopsThumbnailWorkUntilResident)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto artifactPath = ThumbnailPerformanceLibraryArtifactPath(
        "d101000000000000000000000000000000000000000000000000000000000002");
    WriteNativeArtifactTextFile(
        root / artifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        "large-cold-prefab");

    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.subAssetKey = "prefab:LargeCold";
    request.artifactPath = artifactPath;
    request.previewRendererVersion = "large-cold-await-resident:v1";
    request.settingsFingerprint = "large-cold-await-resident";

    LargePrefabAwaitingResidentPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto deferred = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(deferred.has_value());
    EXPECT_EQ(deferred->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(deferred->diagnostic, "thumbnail-prefab-preview-awaiting-resident-load");
    EXPECT_EQ(renderer.pumpCount, 1u);
    EXPECT_EQ(renderer.renderCount, 0u)
        << "A cold large Prefab must stop before mesh/material GPU preparation.";
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_FALSE(service.HasQueuedGpuPreviewResourceContinuation());

    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(repeated.diagnostic, "thumbnail-prefab-preview-awaiting-resident-load");
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u)
        << "Repeated Asset Browser draws must not reactivate deferred cold Prefabs.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, FullReadbackPersistenceQueueKeepsCanonicalGpuPresentation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::ModelPreview;
    request.previewRendererVersion = "direct-gpu-persistence-deferred:v1";
    request.settingsFingerprint = "direct-gpu-persistence-deferred";

    DirectGpuPendingPreviewRenderer renderer;
    renderer.deferPersistence = true;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-persistence-deferred");
    EXPECT_TRUE(generated->gpuTexture.IsValid());
    EXPECT_FALSE(generated->revokeGpuTexture);
    EXPECT_EQ(generated->presentationState, ThumbnailPresentationState::Ready);
    EXPECT_EQ(generated->previewQuality, ThumbnailPreviewQuality::Canonical);
    EXPECT_TRUE(generated->refreshPending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Ready);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, UiLookupExpiresBlockedGpuResourceContinuation)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto artifactPath = ThumbnailPerformanceLibraryArtifactPath(
        "d101000000000000000000000000000000000000000000000000000000000002");
    WriteBinaryFile(
        root / "Assets" / "Models" / "Blocked.fbx",
        std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        root / artifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        "blocked-resource-continuation");

    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.sourceAssetPath = "Assets/Models/Blocked.fbx";
    request.subAssetKey = "prefab:Blocked";
    request.artifactPath = artifactPath;
    request.previewRendererVersion = "blocked-resource-continuation:v1";
    request.settingsFingerprint = "blocked-resource-continuation";

    PrefabBudgetExceededPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    ASSERT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);

    service.SetGpuPreviewResourcePendingAgeForTesting(request, std::chrono::seconds(121));

    // The renderer pump is intentionally not called here. This models a scene
    // resource gate that prevents heavy work from entering GenerateNextThumbnail.
    const auto expired = service.GetThumbnail(request);
    EXPECT_EQ(expired.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_TRUE(expired.diagnostic.rfind(
        "thumbnail-gpu-preview-resources-timeout:", 0u) == 0u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u)
        << "The terminal failure must remain queued until the UI consumes it.";

    const auto terminal = service.ConsumeCompletedThumbnail();
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_TRUE(terminal->diagnostic.rfind(
        "thumbnail-gpu-preview-resources-timeout:", 0u) == 0u);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_FALSE(service.ConsumeCompletedThumbnail().has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, QueuedVisibleGpuPreviewWaitsForFirstSchedulerAdmission)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto artifactPath = ThumbnailPerformanceLibraryArtifactPath(
        "d101000000000000000000000000000000000000000000000000000000000003");
    WriteBinaryFile(
        root / "Assets" / "Models" / "Queued.fbx",
        std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        root / artifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        "queued-resource-continuation");

    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.sourceAssetPath = "Assets/Models/Queued.fbx";
    request.subAssetKey = "prefab:Queued";
    request.artifactPath = artifactPath;
    request.priority = ThumbnailRequestPriority::Visible;
    request.previewRendererVersion = "queued-resource-continuation:v1";
    request.settingsFingerprint = "queued-resource-continuation";

    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_FALSE(service.HasQueuedGpuPreviewResourceContinuation())
        << "Queue admission alone must not start the resource-continuation deadline.";

    service.SetVisibleThumbnailRequestAgeForTesting(
        request,
        std::chrono::seconds(121),
        false);

    service.MaintainPendingThumbnailRequests();
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_FALSE(service.ConsumeCompletedThumbnail(false).has_value())
        << "An untouched queued preview must not publish a timeout before its first scheduler turn.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, LateFreshResultRecoversAfterSameRevisionTimeout)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeTextureRequest(root);
    request.priority = ThumbnailRequestPriority::Visible;
    request.requestRevision = 41u;

    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    std::optional<AssetThumbnailServiceResult> generated;
    generated = service.GenerateNextThumbnail();
    for (int attempt = 0; attempt < 200 && !generated.has_value(); ++attempt)
    {
        generated = service.ConsumeCompletedThumbnail();
        if (!generated.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    service.QueueTerminalAndLateFreshResultForTesting(request);
    const auto timeout = service.ConsumeCompletedThumbnail();
    ASSERT_TRUE(timeout.has_value());
    EXPECT_EQ(timeout->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(timeout->diagnostic, "thumbnail-visible-request-timeout");

    const auto recovered = service.ConsumeCompletedThumbnail();
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(recovered->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(recovered->presentationState, ThumbnailPresentationState::Ready);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Ready);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, NewerRevisionStillRejectsLateFreshAfterTerminalTimeout)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeTextureRequest(root);
    request.priority = ThumbnailRequestPriority::Visible;
    request.requestRevision = 51u;

    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    std::optional<AssetThumbnailServiceResult> generated = service.GenerateNextThumbnail();
    for (int attempt = 0; attempt < 200 && !generated.has_value(); ++attempt)
    {
        generated = service.ConsumeCompletedThumbnail();
        if (!generated.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    service.QueueTerminalAndLateFreshResultForTesting(request);
    auto newerRequest = request;
    newerRequest.requestRevision = 52u;
    (void)service.RequestAssetPreview(newerRequest);

    const auto late = service.ConsumeCompletedThumbnail();
    EXPECT_FALSE(late.has_value())
        << "A late result from a superseded revision must not be published.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ResourceDeadlineSurvivesGenerationScopeSupersede)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto artifactPath = ThumbnailPerformanceLibraryArtifactPath(
        "d101000000000000000000000000000000000000000000000000000000000004");
    WriteBinaryFile(
        root / "Assets" / "Models" / "Superseded.fbx",
        std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        root / artifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        "superseded-resource-continuation");

    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.sourceAssetPath = "Assets/Models/Superseded.fbx";
    request.subAssetKey = "prefab:Superseded";
    request.artifactPath = artifactPath;
    request.priority = ThumbnailRequestPriority::Visible;
    request.previewRendererVersion = "superseded-resource-continuation:v1";
    request.settingsFingerprint = "superseded-resource-continuation";

    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    service.SetGpuPreviewResourcePendingAgeForTesting(request, std::chrono::seconds(121));

    // Asset Browser scope changes cancel queue membership, but the visible
    // request must retain the original resource deadline across the requeue.
    service.SupersedeQueuedRequestsForGeneration("scope-after-scroll");

    const auto expired = service.GetThumbnail(request);
    EXPECT_EQ(expired.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_TRUE(expired.diagnostic.rfind(
        "thumbnail-gpu-preview-resources-timeout:", 0u) == 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ResolvedArtifactRequestCoalescesWithUnresolvedActiveRequest)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto unresolved = MakeGpuPreviewRequest(root);
    unresolved.subAssetKey = "prefab:Hero";
    unresolved.kind = AssetThumbnailKind::PrefabPreview;
    unresolved.previewRendererVersion = "coalescing-preview:v1";
    unresolved.settingsFingerprint = "coalescing-preview";

    AssetThumbnailService service;
    const auto first = service.GetThumbnail(unresolved);
    ASSERT_EQ(first.status, AssetThumbnailServiceStatus::Pending);
    ASSERT_NE(first.requestRevision, 0u);
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u);

    auto resolved = unresolved;
    resolved.artifactPath =
        (root / "Library" / "Artifacts" / "resolved-prefab.artifact").generic_string();
    const auto second = service.GetThumbnail(resolved);
    EXPECT_EQ(second.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(second.requestRevision, first.requestRevision);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u)
        << "Manifest resolution must not create a second preparation owner.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ResourceDeadlineUsesStablePresentationAcrossArtifactResolution)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto unresolved = MakeGpuPreviewRequest(root);
    unresolved.kind = AssetThumbnailKind::PrefabPreview;
    unresolved.subAssetKey = "prefab:ResolvedDeadline";
    unresolved.priority = ThumbnailRequestPriority::Visible;
    unresolved.previewRendererVersion = "stable-resource-deadline:v1";
    unresolved.settingsFingerprint = "stable-resource-deadline";

    AssetThumbnailService service;
    const auto first = service.GetThumbnail(unresolved);
    ASSERT_EQ(first.status, AssetThumbnailServiceStatus::Pending);
    service.SetGpuPreviewResourcePendingAgeForTesting(unresolved, std::chrono::seconds(121));

    auto resolved = unresolved;
    resolved.artifactPath =
        (root / "Library" / "Artifacts" / "resolved-deadline.artifact").generic_string();
    const auto expired = service.GetThumbnail(resolved);
    EXPECT_EQ(expired.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_TRUE(expired.diagnostic.rfind(
        "thumbnail-gpu-preview-resources-timeout:", 0u) == 0u);
    EXPECT_EQ(service.GetThumbnailState(resolved), ThumbnailState::Failed);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPreviewRejectsFullyTransparentReadbackEvenWhenRgbVaries)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    TransparentVaryingRgbPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-empty-frame");
    EXPECT_FALSE(service.HasInFlightRequest());
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, PrefabEmptyGpuFrameFallsBackToCanonicalCpuThumbnail)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Serialize;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("bfa81818-1818-4818-8818-181818181818"));
    const auto sourcePath = root / "Assets" / "Prefabs" / "CpuFallback.prefab";
    const auto artifactPath = root / "Library" / "Artifacts" / "CpuFallback.nprefab";
    WriteBinaryFile(sourcePath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.documentId = NLS::Guid::NewDeterministic(
        "AssetThumbnailBehaviorTests.CpuFallbackPrefab.Document");
    const auto rootObjectId = MakeObjectId("cfa81818-1818-4818-8818-181818181818");
    document.root = rootObjectId;
    document.objects.push_back({
        rootObjectId,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
        "CpuFallbackRoot",
        "CpuFallbackRoot",
        ObjectRecordState::Alive,
        {},
        MakeLocalIdentifierInFile(rootObjectId)});
    WriteNativeArtifactTextFile(
        artifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        ObjectGraphWriter::Write(document));

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Prefabs/CpuFallback.prefab";
    request.subAssetKey = "prefab:CpuFallback";
    request.artifactPath = artifactPath.generic_string();
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.previewRendererVersion = "empty-frame-cpu-fallback:v1";
    request.settingsFingerprint = "empty-frame-cpu-fallback";
    request.freshnessInputs = {{"artifact", "cpu-fallback:v1"}};

    TransparentVaryingRgbPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto gpuResult = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(gpuResult.has_value());
    EXPECT_EQ(gpuResult->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(
        gpuResult->diagnostic,
        "thumbnail-gpu-preview-empty-frame-cpu-fallback-pending");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Preparing);
    EXPECT_EQ(renderer.renderCount, 1u);
    EXPECT_FALSE(std::filesystem::exists(gpuResult->imagePath));

    std::optional<AssetThumbnailServiceResult> completed;
    for (size_t attempt = 0u; attempt < 200u && !completed.has_value(); ++attempt)
    {
        completed = service.ConsumeCompletedThumbnail();
        if (!completed.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(completed->presentationState, ThumbnailPresentationState::Ready);
    ASSERT_TRUE(completed->cacheEntry.has_value());
    EXPECT_TRUE(std::filesystem::exists(completed->cacheEntry->imagePath));
    EXPECT_EQ(
        EvaluateAssetThumbnailCache(request).status,
        AssetThumbnailCacheStatus::Fresh);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Ready);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, LargePrefabEmptyGpuFrameDoesNotScheduleCpuArtifactFallback)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto sourcePath = root / "Assets" / "Prefabs" / "LargeResident.prefab";
    const auto artifactPath = root / "Library" / "Artifacts" / "LargeResident.nprefab";
    WriteBinaryFile(sourcePath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteBinaryFile(artifactPath, std::vector<uint8_t>{'a', 'r', 't', 'i', 'f', 'a', 'c', 't'});

    auto request = MakeGpuPreviewRequest(root);
    request.sourceAssetPath = "Assets/Prefabs/LargeResident.prefab";
    request.subAssetKey = "prefab:LargeResident";
    request.artifactPath = artifactPath.generic_string();
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.previewRendererVersion = "large-empty-frame-no-cpu-fallback:v1";
    request.settingsFingerprint = "large-empty-frame-no-cpu-fallback";

    TransparentVaryingRgbPreviewRenderer renderer(
        kMaxColdGpuPreviewPrefabDrawItems + 1u);
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-empty-frame");
    EXPECT_FALSE(service.HasInFlightRequest());
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, OpaqueBlackPrefabPreviewDefersUntilScopeCancellation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.sourceAssetPath = "Assets/Prefabs/Dark.prefab";
    request.subAssetKey.clear();
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.previewRendererVersion = "opaque-black-prefab-preview:v1";

    OpaqueBlackPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto deferred = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(deferred.has_value());
    EXPECT_EQ(deferred->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(deferred->diagnostic, "thumbnail-gpu-preview-empty-frame");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing)
        << "A deferred opaque frame must not write failed cache metadata.";
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u)
        << "A deferred opaque frame is retained for a later scope, not left pumpable.";

    EXPECT_FALSE(service.GenerateNextThumbnail(renderer, true).has_value());
    EXPECT_EQ(renderer.renderCount, 1u)
        << "A deferred opaque frame must not hot-retry in the same generation.";

    service.SupersedeQueuedRequestsForGeneration("opaque-black-prefab-cancelled-scope");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Cancelled);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPreviewRendererDoesNotSynchronouslyLoadUncachedMeshArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("31313131-3131-4131-8131-313131313131"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    ScopedServiceOverride meshManagerOverride(meshManager);
    ScopedServiceOverride materialManagerOverride(materialManager);
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.subAssetKey = "mesh:Body";
    request.artifactPath = "Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh";
    request.kind = AssetThumbnailKind::ModelPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:no-sync-load";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {{"artifact", "uncached-mesh:v1"}};

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    EditorThumbnailPreviewRenderer renderer(EnsureThumbnailPerformanceTestDriver());
    const auto rendered = renderer.Render(request);

    EXPECT_TRUE(rendered.rgbaPixels.empty());
    ExpectResourcesPendingDiagnostic(rendered.diagnostic);

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
            "meshes/Body.nmesh"),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize,
            "meshes/Body.nmesh"),
        0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPreviewRendererDoesNotSynchronouslyLoadUncachedMaterialArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("32323232-3232-4232-8232-323232323232"));
    const auto materialArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("d001000000000000000000000000000000000000000000000000000000000001");
    const auto materialPath = root / materialArtifactPath;
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");

    NLS::Core::ResourceManagement::MeshManager meshManager;
    CountingMaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    ScopedServiceOverride meshManagerOverride(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerOverride(materialManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::ShaderManager> shaderManagerOverride(shaderManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::TextureManager> textureManagerOverride(textureManager);
    const auto repositoryEngineAssetsRoot =
        std::filesystem::current_path() / "App" / "Assets" / "Engine";
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", repositoryEngineAssetsRoot);

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.subAssetKey = "material:Body";
    request.artifactPath = materialArtifactPath;
    request.kind = AssetThumbnailKind::MaterialSphere;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:no-sync-material-load";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {{"artifact", "uncached-material:v1"}};

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    EditorThumbnailPreviewRenderer renderer(EnsureThumbnailPerformanceTestDriver());
    const auto rendered = renderer.Render(request);

    EXPECT_TRUE(rendered.rgbaPixels.empty());
    ExpectResourcesPendingDiagnostic(rendered.diagnostic);
    EXPECT_EQ(materialManager.prewarmWithDependenciesCount, 0u);
    EXPECT_EQ(materialManager.asyncRequestCount, 1u);
    EXPECT_TRUE(ContainsPathWithSuffix(
        materialManager.asyncRequestPaths,
        std::filesystem::path(materialArtifactPath).filename().generic_string()));

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    const auto materialArtifactFileName = std::filesystem::path(materialArtifactPath).filename().generic_string();
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
            materialArtifactFileName),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize,
            materialArtifactFileName),
        0u);
    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPrefabPreviewDetectsTerminalAsyncMeshFailureWithoutSynchronousPrewarm)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto assetId = prefab.assetId;
    const auto prefabPayload = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    const auto prefabArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("a001000000000000000000000000000000000000000000000000000000000001");
    const auto meshArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("a002000000000000000000000000000000000000000000000000000000000002");
    const auto materialArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("a003000000000000000000000000000000000000000000000000000000000003");
    const auto meshId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("50505050-5050-4050-8050-505050505050"));
    const auto materialId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("60606060-6060-4060-8060-606060606060"));
    prefab.resolvedAssets[0].artifactPath = meshArtifactPath;
    prefab.resolvedAssets[1].artifactPath = materialArtifactPath;
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        root / meshArtifactPath,
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    WriteNativeArtifactTextFile(
        root / prefabArtifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteNativeArtifactTextFile(
        root / materialArtifactPath,
        ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = assetId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        assetId,
        "prefab:Hero",
        ArtifactType::Prefab,
        "native-prefab",
        prefabArtifactPath,
        "prefab-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        meshId,
        "mesh:Hero",
        ArtifactType::Mesh,
        "mesh",
        meshArtifactPath,
        "mesh-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        materialId,
        "material:Hero",
        ArtifactType::Material,
        "native-material",
        materialArtifactPath,
        "material-hash"));
    WriteThumbnailPerformanceArtifactDatabase(root, manifest);

    auto importedFixture = NLS::Engine::Assets::ImportPrefabArtifact(
        prefabPayload,
        assetId,
        prefab.resolvedAssets);
    ASSERT_FALSE(importedFixture.diagnostics.HasErrors());
    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    auto loadedFixture = database.LoadPrefabArtifactByAssetId(assetId, "prefab:Hero");
    ASSERT_TRUE(loadedFixture.has_value());

    CountingMeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerOverride(meshManager);
    ScopedServiceOverride materialManagerOverride(materialManager);
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.subAssetKey = "prefab:Hero";
    request.artifactPath = prefabArtifactPath;
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:no-sync-prefab-mesh-load";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {{"artifact", "uncached-prefab-mesh:v1"}};

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    ResetThumbnailPerformanceJobSystem();
    EditorThumbnailPreviewRenderer renderer(EnsureThumbnailPerformanceTestDriver());
    const auto unavailableJobSystemPump = renderer.PumpResources(request);
    EXPECT_TRUE(unavailableJobSystemPump.resourcesPending);
    EXPECT_EQ(
        unavailableJobSystemPump.diagnostic,
        "thumbnail-gpu-preview-resources-pending:prefab-prepare-job-system=0");
    EXPECT_EQ(meshManager.asyncRequestCount, 0u);

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    EditorThumbnailPreviewResult rendered;
    for (size_t attempt = 0u; attempt < 128u && meshManager.asyncRequestCount == 0u; ++attempt)
    {
        rendered = renderer.Render(request);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(rendered.rgbaPixels.empty());
    ExpectResourcesPendingDiagnostic(rendered.diagnostic);
    EXPECT_TRUE(rendered.resourceWorkActive)
        << "An accepted asynchronous mesh request must keep the continuation deadline alive while its counts are unchanged.";
    EXPECT_EQ(meshManager.prewarmCount, 0u);
    EXPECT_EQ(meshManager.asyncRequestCount, 1u);
    EXPECT_EQ(meshManager.lastAsyncPath, (root / meshArtifactPath).generic_string());

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
            std::filesystem::path(meshArtifactPath).filename().generic_string()),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize,
            std::filesystem::path(meshArtifactPath).filename().generic_string()),
        0u);

#if defined(NLS_ENABLE_TEST_HOOKS)
    std::optional<NLS::Core::ResourceManagement::MeshManager> meshManagerStorage;
    meshManagerStorage.emplace();
    const auto* failingMeshManagerAddress = &*meshManagerStorage;
    auto corruptMeshArtifact = NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact());
    ASSERT_GE(corruptMeshArtifact.size(), 64u);
    uint64_t payloadOffset = 0u;
    std::memcpy(&payloadOffset, corruptMeshArtifact.data() + 40u, sizeof(payloadOffset));
    ASSERT_LE(payloadOffset + 20u, corruptMeshArtifact.size());
    const uint32_t inconsistentIndexCount = 6u;
    std::memcpy(
        corruptMeshArtifact.data() + payloadOffset + 16u,
        &inconsistentIndexCount,
        sizeof(inconsistentIndexCount));
    // Header preview remains readable, while the full loader rejects the payload-size mismatch.
    WriteBinaryFile(root / meshArtifactPath, corruptMeshArtifact);
    ASSERT_TRUE(NLS::Render::Assets::ReadMeshArtifactHeaderPreview(root / meshArtifactPath).has_value());
    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    {
        auto& failingMeshManager = *meshManagerStorage;
        ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> failingMeshManagerOverride(
            failingMeshManager);

        EditorThumbnailPreviewResourcePumpResult terminal;
        for (size_t attempt = 0u; attempt < 128u; ++attempt)
        {
            terminal = renderer.PumpResources(request);
            if (terminal.diagnostic.rfind("thumbnail-gpu-preview-mesh-load-failed", 0u) == 0u)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        EXPECT_TRUE(terminal.supported);
        EXPECT_FALSE(terminal.resourcesPending);
        EXPECT_EQ(terminal.diagnostic, "thumbnail-gpu-preview-mesh-load-failed|meshFailed=1");
        EXPECT_FALSE(failingMeshManager.IsAsyncArtifactLoadPending((root / meshArtifactPath).generic_string()));
        EXPECT_TRUE(failingMeshManager.IsAsyncArtifactLoadFailed((root / meshArtifactPath).generic_string()));
        EXPECT_EQ(
            NLS::Core::ResourceManagement::MeshManager::GetFailedAsyncArtifactRequestCountForTesting(),
            1u);

        const auto failedRequestCount =
            NLS::Core::ResourceManagement::MeshManager::GetTotalAsyncArtifactRequestCountForTesting();
        const auto repeated = renderer.PumpResources(request);
        EXPECT_EQ(repeated.diagnostic, terminal.diagnostic);
        EXPECT_EQ(
            NLS::Core::ResourceManagement::MeshManager::GetTotalAsyncArtifactRequestCountForTesting(),
            failedRequestCount)
            << "A terminal mesh failure must not be re-requested until the request key changes.";

        const auto failedRender = renderer.Render(request);
        EXPECT_TRUE(failedRender.rgbaPixels.empty());
        EXPECT_EQ(failedRender.diagnostic, terminal.diagnostic);
        EXPECT_EQ(failedRender.rawVisibleDrawCount, 0u);
        EXPECT_EQ(failedRender.submittedSceneDrawCount, 0u);

        NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
        failingMeshManager.UnloadResources();
    }
    meshManagerStorage.reset();

    WriteBinaryFile(
        root / meshArtifactPath,
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    meshManagerStorage.emplace();
    ASSERT_EQ(&*meshManagerStorage, failingMeshManagerAddress);
    {
        auto& recoveredMeshManager = *meshManagerStorage;
        ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> recoveredMeshManagerOverride(
            recoveredMeshManager);
        const auto readyMeshArtifact = TriangleMeshArtifact();
        recoveredMeshManager.RegisterResource(
            (root / meshArtifactPath).generic_string(),
            new NLS::Render::Resources::Mesh(
                readyMeshArtifact.vertices,
                readyMeshArtifact.indices,
                readyMeshArtifact.materialIndex,
                NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
                readyMeshArtifact.boundingSphere));

        auto recovered = renderer.PumpResources(request);
        // The mesh manager is restored synchronously, but the material
        // manager may still be finishing the request started before the
        // manager swap. A bounded resource pump must not synchronously wait
        // for that unrelated background completion, so allow it to retire
        // before asserting the recovered state.
        for (size_t attempt = 0u;
            attempt < 128u && recovered.resourcesPending;
            ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            recovered = renderer.PumpResources(request);
        }
        EXPECT_TRUE(recovered.supported);
        EXPECT_FALSE(recovered.resourcesPending) << recovered.diagnostic;
        EXPECT_TRUE(recovered.diagnostic.empty()) << recovered.diagnostic;
        recoveredMeshManager.UnloadResources();
    }
#endif

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPrefabPreviewReusesSnapshotWhileResourcesArePending)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto assetId = prefab.assetId;
    const auto prefabPayload = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    const auto prefabArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("b001000000000000000000000000000000000000000000000000000000000001");
    const auto meshArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("b002000000000000000000000000000000000000000000000000000000000002");
    const auto materialArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("b003000000000000000000000000000000000000000000000000000000000003");
    const auto meshId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("50505050-5050-4050-8050-505050505050"));
    const auto materialId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("60606060-6060-4060-8060-606060606060"));
    prefab.resolvedAssets[0].artifactPath = meshArtifactPath;
    prefab.resolvedAssets[1].artifactPath = materialArtifactPath;
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        root / meshArtifactPath,
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteNativeArtifactTextFile(
        root / prefabArtifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteNativeArtifactTextFile(
        root / materialArtifactPath,
        ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = assetId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        assetId,
        "prefab:Hero",
        ArtifactType::Prefab,
        "native-prefab",
        prefabArtifactPath,
        "prefab-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        meshId,
        "mesh:Hero",
        ArtifactType::Mesh,
        "mesh",
        meshArtifactPath,
        "mesh-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        materialId,
        "material:Hero",
        ArtifactType::Material,
        "native-material",
        materialArtifactPath,
        "material-hash"));
    WriteThumbnailPerformanceArtifactDatabase(root, manifest);

    CountingMeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerOverride(meshManager);
    ScopedServiceOverride materialManagerOverride(materialManager);
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.subAssetKey = "prefab:Hero";
    request.artifactPath = prefabArtifactPath;
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:snapshot-cache-pending";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {
        {"artifact", "snapshot-cache:v1"},
        {"dependency", "mesh-material:v1"}
    };

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    EditorThumbnailPreviewRenderer renderer(EnsureThumbnailPerformanceTestDriver());
    EditorThumbnailPreviewResult first;
    for (size_t attempt = 0u; attempt < 128u && meshManager.asyncRequestCount == 0u; ++attempt)
    {
        first = renderer.Render(request);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(first.rgbaPixels.empty());
    ExpectResourcesPendingDiagnostic(first.diagnostic);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    std::filesystem::remove(root / prefabArtifactPath);

    const auto second = renderer.Render(request);

    EXPECT_TRUE(second.rgbaPixels.empty());
    ExpectResourcesPendingDiagnostic(second.diagnostic);
    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
            std::filesystem::path(prefabArtifactPath).filename().generic_string()),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources,
            "Assets/Models/Hero.fbx|prefab:Hero"),
        0u)
        << "A cached prefab preview snapshot with pending dependencies should only pump those dependencies, "
           "not rescan and resolve every draw item again on the UI thread.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ResidentPrefabPreviewUsesReadyResourcesWithoutArtifactOrAsyncLoads)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    CountingMeshManager meshManager;
    CountingMaterialManager materialManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerOverride(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerOverride(materialManager);
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());

    const auto meshPath = std::string(":resident/thumbnail-mesh");
    const auto materialPath = std::string(":resident/thumbnail-material");
    auto& driver = EnsureThumbnailPerformanceTestDriver();
    const auto meshArtifact = TriangleMeshArtifact();
    meshManager.RegisterResource(
        meshPath,
        new NLS::Render::Resources::Mesh(
            meshArtifact.vertices,
            meshArtifact.indices,
            meshArtifact.materialIndex,
            NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
            meshArtifact.boundingSphere));
    materialManager.RegisterResource(materialPath, new NLS::Render::Resources::Material());

    PreviewRenderableSnapshot snapshot;
    PreviewDrawItem drawItem;
    drawItem.meshPath = meshPath;
    drawItem.materialPaths.push_back(materialPath);
    snapshot.drawItems.push_back(std::move(drawItem));
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();

    auto registry = ResidentPrefabPreviewRegistry::Create();
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry resourceLifetimeRegistry;
    const auto runtimeIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        "41414141-4141-4141-8141-414141414141",
        "prefab:Resident");
    const auto freshness = std::string("resident-ready:v1");
    const auto resourceOwnerToken = std::string("test-resident-thumbnail");
    auto residentResources = std::make_shared<ResidentPrefabPreviewResources>();
    residentResources->meshManagerInstanceId = meshManager.GetInstanceId();
    residentResources->materialManagerInstanceId = materialManager.GetInstanceId();
    residentResources->textureManagerInstanceId = 0u;
    auto meshHandle = meshManager.AcquireRegisteredMeshHandle(
        resourceLifetimeRegistry,
        resourceOwnerToken,
        meshPath,
        NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);
    auto materialHandle = materialManager.AcquireRegisteredMaterialHandle(
        resourceLifetimeRegistry,
        resourceOwnerToken,
        materialPath,
        NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);
    ASSERT_TRUE(meshHandle);
    ASSERT_TRUE(materialHandle);
    residentResources->meshes.push_back(std::move(meshHandle));
    residentResources->materials.push_back(std::move(materialHandle));
    residentResources->meshIndicesByPath.emplace(meshPath, 0u);
    residentResources->materialIndicesByPath.emplace(materialPath, 0u);
    residentResources->drawItems.push_back({0u, {0u}});
    registry->RegisterSnapshot(
        runtimeIdentity,
        freshness,
        std::make_shared<const PreviewRenderableSnapshot>(snapshot),
        1024u,
        false,
        {},
        {},
        residentResources);

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("41414141-4141-4141-8141-414141414141"));
    request.sourceAssetPath = "Assets/Prefabs/Resident.prefab";
    request.subAssetKey = "prefab:Resident";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "resident-ready:v1";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {{"resident", freshness}};
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        runtimeIdentity,
        freshness,
        {},
        registry
    };

    EditorThumbnailPreviewRenderer renderer(driver);
    EditorThumbnailPreviewResourcePumpResult pump;
    bool ready = false;
    for (size_t attempt = 0u; attempt < 256u; ++attempt)
    {
        pump = renderer.PumpResources(request);
        if (!pump.resourcesPending && pump.diagnostic.empty())
        {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_TRUE(ready) << pump.diagnostic;
    EXPECT_EQ(meshManager.asyncRequestCount, 0u);
    EXPECT_EQ(meshManager.prewarmCount, 0u);
    EXPECT_EQ(materialManager.asyncRequestCount, 0u);
    EXPECT_EQ(materialManager.prewarmWithDependenciesCount, 0u);
    const auto stats = registry->GetStats();
    EXPECT_GE(stats.thumbnailZeroArtifactReadHitCount, 1u);
    EXPECT_EQ(stats.thumbnailMissCount, 0u);

    // Scene restore can rebuild an equivalent package with fresh handles. The
    // registry must retain the first package so the renderer does not restart
    // its resource plan for a no-op registration.
    auto equivalentResources = std::make_shared<ResidentPrefabPreviewResources>();
    equivalentResources->meshManagerInstanceId = meshManager.GetInstanceId();
    equivalentResources->materialManagerInstanceId = materialManager.GetInstanceId();
    equivalentResources->textureManagerInstanceId = 0u;
    auto equivalentMeshHandle = meshManager.AcquireRegisteredMeshHandle(
        resourceLifetimeRegistry,
        "test-resident-thumbnail-equivalent",
        meshPath,
        NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);
    auto equivalentMaterialHandle = materialManager.AcquireRegisteredMaterialHandle(
        resourceLifetimeRegistry,
        "test-resident-thumbnail-equivalent",
        materialPath,
        NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);
    ASSERT_TRUE(equivalentMeshHandle);
    ASSERT_TRUE(equivalentMaterialHandle);
    equivalentResources->meshes.push_back(std::move(equivalentMeshHandle));
    equivalentResources->materials.push_back(std::move(equivalentMaterialHandle));
    equivalentResources->meshIndicesByPath.emplace(meshPath, 0u);
    equivalentResources->materialIndicesByPath.emplace(materialPath, 0u);
    equivalentResources->drawItems.push_back({0u, {0u}});
    registry->RegisterSnapshot(
        runtimeIdentity,
        freshness,
        std::make_shared<const PreviewRenderableSnapshot>(snapshot),
        1024u,
        false,
        {},
        {},
        equivalentResources);
    const auto equivalentLease = registry->Acquire(runtimeIdentity, freshness);
    ASSERT_TRUE(equivalentLease.has_value());
    EXPECT_EQ(equivalentLease->Resources(), residentResources);

    NLS::Core::ResourceManagement::MeshManager replacementMeshManager;
    EXPECT_FALSE(residentResources->IsValidFor(
        replacementMeshManager,
        materialManager,
        nullptr))
        << "A resident package must not cross resource-manager generations.";

    auto replacementSnapshot = snapshot;
    replacementSnapshot.drawItems.front().meshPath = ":resident/replacement-mesh";
    registry->RegisterSnapshot(
        runtimeIdentity,
        freshness,
        std::make_shared<const PreviewRenderableSnapshot>(replacementSnapshot),
        1024u);
    const auto replacementLease = registry->Acquire(runtimeIdentity, freshness);
    ASSERT_TRUE(replacementLease.has_value());
    EXPECT_EQ(replacementLease->Resources(), nullptr)
        << "Replacing a snapshot without a matching package must invalidate the old package.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ResidentPrefabPreviewReusesTransientSceneMeshWithoutManagerRegistration)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    CountingMeshManager meshManager;
    CountingMaterialManager materialManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerOverride(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerOverride(materialManager);
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());

    const auto meshPath = std::string(":scene/transient-mesh");
    const auto materialPath = std::string(":scene/transient-material");
    materialManager.RegisterResource(materialPath, new NLS::Render::Resources::Material());

    PreviewRenderableSnapshot snapshot;
    PreviewDrawItem drawItem;
    drawItem.meshPath = meshPath;
    drawItem.materialPaths.push_back(materialPath);
    snapshot.drawItems.push_back(std::move(drawItem));
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();

    auto registry = ResidentPrefabPreviewRegistry::Create();
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry resourceLifetimeRegistry;
    const auto runtimeIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        "42424242-4242-4242-8242-424242424242",
        "prefab:Transient");
    const auto freshness = std::string("resident-transient:v1");
    auto residentResources = std::make_shared<ResidentPrefabPreviewResources>();
    residentResources->meshManagerInstanceId = meshManager.GetInstanceId();
    residentResources->materialManagerInstanceId = materialManager.GetInstanceId();
    residentResources->textureManagerInstanceId = 0u;
    residentResources->meshes.emplace_back();
    auto transientMesh = MakeOpaqueThumbnailGpuResource<
        NLS::Render::Resources::Mesh>();
    residentResources->transientMeshesByIndex.emplace(
        0u,
        std::move(transientMesh));
    auto materialHandle = materialManager.AcquireRegisteredMaterialHandle(
        resourceLifetimeRegistry,
        "test-resident-transient-thumbnail",
        materialPath,
        NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);
    ASSERT_TRUE(materialHandle);
    residentResources->materials.push_back(std::move(materialHandle));
    residentResources->meshIndicesByPath.emplace(meshPath, 0u);
    residentResources->materialIndicesByPath.emplace(materialPath, 0u);
    residentResources->drawItems.push_back({0u, {0u}});
    ASSERT_TRUE(residentResources->IsValidFor(meshManager, materialManager, nullptr));
    registry->RegisterSnapshot(
        runtimeIdentity,
        freshness,
        std::make_shared<const PreviewRenderableSnapshot>(snapshot),
        1024u,
        false,
        {},
        {},
        residentResources);

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("42424242-4242-4242-8242-424242424242"));
    request.sourceAssetPath = "Assets/Prefabs/Transient.prefab";
    request.subAssetKey = "prefab:Transient";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "resident-transient:v1";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {{"resident", freshness}};
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        runtimeIdentity,
        freshness,
        {},
        registry};

    auto residentLease = registry->Acquire(runtimeIdentity, freshness, true);
    ASSERT_TRUE(residentLease.has_value());
    ASSERT_TRUE(residentLease->Resources() != nullptr);
    EXPECT_TRUE(residentLease->Resources()->IsValidFor(meshManager, materialManager, nullptr));
    EXPECT_EQ(meshManager.asyncRequestCount, 0u);
    EXPECT_EQ(meshManager.prewarmCount, 0u);
    EXPECT_EQ(materialManager.asyncRequestCount, 0u);
    EXPECT_EQ(materialManager.prewarmWithDependenciesCount, 0u);
    std::filesystem::remove_all(root);
    residentLease.reset();
    residentResources.reset();
    materialHandle.Reset();
    registry.reset();
}

TEST(AssetThumbnailBehaviorTests, LiveScenePrefabResourcesAttachWithoutArtifactLoads)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    CountingMeshManager meshManager;
    CountingMaterialManager materialManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerOverride(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerOverride(materialManager);
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    (void)EnsureThumbnailPerformanceTestDriver();

    const auto meshPath = std::string(
        "Library/Artifacts/50505050-5050-4050-8050-505050505050/Hero.nmesh");
    const auto materialPath = std::string("Assets/Materials/Hero.mat");
    const auto meshArtifact = TriangleMeshArtifact();
    auto* mesh = meshManager.RegisterResource(
        meshPath,
        new NLS::Render::Resources::Mesh(
            meshArtifact.vertices,
            meshArtifact.indices,
            meshArtifact.materialIndex,
            NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
            meshArtifact.boundingSphere));
    auto* material = materialManager.RegisterResource(
        materialPath,
        new NLS::Render::Resources::Material());
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(material, nullptr);

    NLS::Engine::SceneSystem::Scene scene;
    auto& liveObject = scene.CreateGameObject("ResidentLiveObject");
    auto* meshFilter = liveObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = liveObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);
    meshFilter->SetModelPathHint(meshPath);
    meshRenderer->SetMaterialAtIndex(0u, *material);

    const auto sourceObject = MakeObjectId("70707070-7070-4070-8070-707070707070");
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("52525252-5252-4252-8252-525252525252"));
    auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    prefab.assetId = assetId;
    const auto sourceAssetPath = std::string(
        "Assets/Models/NewSponza_Main_glTF_003.gltf");
    const auto importerSubAssetKey = std::string(
        "model:NewSponza_Main_glTF_003");
    const auto canonicalSubAssetKey = std::string(
        "prefab:NewSponza_Main_glTF_003");
    const auto runtimeIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        assetId.ToString(),
        canonicalSubAssetKey);
    auto registry = ResidentPrefabPreviewRegistry::Create();

    NLS::Core::ResourceManagement::ResourceLifetimeRegistry resourceLifetimeRegistry;
    const std::unordered_map<
        const NLS::Engine::GameObject*,
        NLS::Engine::Serialize::ObjectId> sourceByInstanceObject {
            {&liveObject, sourceObject}
        };
    auto sceneLease = registry->EnsureLivePrefabSnapshotForScene(
        root,
        assetId,
        sourceAssetPath,
        importerSubAssetKey,
        {},
        {},
        prefab,
        sourceByInstanceObject,
        meshManager,
        materialManager,
        nullptr,
        resourceLifetimeRegistry);
    ASSERT_TRUE(sceneLease.has_value())
        << "An already-loaded instance must create the Resident snapshot when the registry is empty.";
    EXPECT_TRUE(registry->HasSnapshotForRuntimeCacheIdentity(runtimeIdentity));

    // A real Asset Browser row can resolve a different artifact-freshness
    // spelling after the scene registered. The live scene lease is
    // authoritative for this stable asset/sub-asset identity.
    const auto requestFreshness = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourceAssetPath,
        importerSubAssetKey,
        "Library/Artifacts/live-scene-prefab.nprefab");
    auto lease = registry->Acquire(runtimeIdentity, requestFreshness, true);
    ASSERT_TRUE(lease.has_value());
    ASSERT_NE(lease->Resources(), nullptr);
    ASSERT_EQ(lease->Resources()->drawItems.size(), 1u);
    EXPECT_TRUE(lease->Resources()->IsCompleteForSource());
    ASSERT_EQ(lease->Resources()->meshes.size(), 1u);
    ASSERT_EQ(lease->Resources()->materials.size(), 1u);
    EXPECT_EQ(lease->Resources()->meshes.front().Get(), mesh);
    EXPECT_EQ(lease->Resources()->materials.front().Get(), material);
    EXPECT_EQ(meshManager.asyncRequestCount, 0u);
    EXPECT_EQ(meshManager.prewarmCount, 0u);
    EXPECT_EQ(materialManager.asyncRequestCount, 0u);
    EXPECT_EQ(materialManager.prewarmWithDependenciesCount, 0u);
    const auto stats = registry->GetStats();
    EXPECT_EQ(stats.thumbnailHitCount, 1u);
    EXPECT_EQ(stats.thumbnailZeroArtifactReadHitCount, 1u);
    EXPECT_EQ(stats.thumbnailMissCount, 0u);

    std::filesystem::remove_all(root);
    lease.reset();
    sceneLease.reset();
    registry.reset();
}

TEST(AssetThumbnailBehaviorTests, RegisteredResourceHandleProbeNeverStartsMissingLoad)
{
    CountingMeshManager meshManager;
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry resourceLifetimeRegistry;

    const auto handle = meshManager.AcquireRegisteredMeshHandle(
        resourceLifetimeRegistry,
        "test-resident-probe",
        ":resident/missing-mesh",
        NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);

    EXPECT_FALSE(handle);
    EXPECT_EQ(meshManager.asyncRequestCount, 0u);
    EXPECT_EQ(meshManager.prewarmCount, 0u);
}

TEST(AssetThumbnailBehaviorTests, ResidentPrefabPreviewResolutionSkipsArtifactManifestLookup)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto registry = ResidentPrefabPreviewRegistry::Create();

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("51515151-5151-4151-8151-515151515151"));
    request.sourceAssetPath = "Assets/Prefabs/Resident.prefab";
    request.subAssetKey = "prefab:Resident";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.dependencyStamp = "source=v1;artifact-db=v1;";
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        BuildResidentPrefabRuntimeCacheIdentity(
            request.assetId.ToString(),
            request.subAssetKey),
        request.dependencyStamp,
        {},
        registry
    };

    ResetAssetThumbnailManifestLookupStatsForTesting();
    const auto resolved = ResolveDeferredThumbnailPreviewRequestForTesting(request);
    const auto stats = GetAssetThumbnailManifestLookupStatsForTesting();

    EXPECT_TRUE(resolved.artifactPath.empty());
    ASSERT_TRUE(resolved.residentPrefabPreviewSource.has_value());
    EXPECT_EQ(
        resolved.residentPrefabPreviewSource->freshnessFingerprint,
        request.dependencyStamp);
    EXPECT_EQ(stats.lookupCount, 0u)
        << "A resident thumbnail request must reach the registry before any ArtifactDB manifest read.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ResidentModelAliasKeepsImporterFreshnessAndCanonicalRuntimeIdentity)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("61616161-6161-4161-8161-616161616161"));
    const auto sourcePath = std::string("Assets/Models/Resident.gltf");
    const auto artifactPath = std::string("Library/Artifacts/resident.nprefab");
    WriteBinaryFile(root / sourcePath, std::vector<uint8_t>{'g', 'l', 't', 'f'});
    WriteBinaryFile(root / artifactPath, std::vector<uint8_t>{'a', 'r', 't'});

    AssetBrowserItem item;
    item.type = AssetBrowserItemType::Model;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.assetId = assetId;
    item.sourceAssetPath = sourcePath;
    item.subAssetKey = "model:Resident";
    item.artifactPath = artifactPath;

    AssetThumbnailRequestBuildContext context;
    context.featureConfig.residentPrefabPreview = true;
    context.residentPrefabPreviewRegistry = ResidentPrefabPreviewRegistry::Create();
    context.deferManifestLookups = true;

    const auto request = BuildAssetThumbnailRequestForItem(root, item, 64u, context);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(request->residentPrefabPreviewSource.has_value());

    const auto canonicalFreshness = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourcePath,
        "prefab:Resident",
        artifactPath);
    EXPECT_EQ(
        request->residentPrefabPreviewSource->runtimeCacheIdentity,
        BuildResidentPrefabRuntimeCacheIdentity(assetId.ToString(), "prefab:Resident"));
    EXPECT_EQ(
        request->residentPrefabPreviewSource->freshnessFingerprint,
        request->dependencyStamp);
    EXPECT_NE(request->dependencyStamp, canonicalFreshness);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ResidentVisibleUpgradeRepairsQueuedLane)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto request = MakeGpuPreviewRequest(root);
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.subAssetKey = "prefab:Hero";
    request.priority = ThumbnailRequestPriority::Visible;

    AssetThumbnailFeatureConfig featureConfig;
    featureConfig.explicitLanes = true;
    AssetThumbnailService service(featureConfig);

    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_FALSE(service.HasQueuedVisibleResidentThumbnail());

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    const auto runtimeIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        request.assetId.ToString(),
        request.subAssetKey);
    const auto freshness = std::string("source:v1");
    const auto residentSnapshot = std::make_shared<const PreviewRenderableSnapshot>();
    registry->RegisterSnapshot(
        runtimeIdentity,
        freshness,
        residentSnapshot,
        1024u);

    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        runtimeIdentity,
        freshness,
        residentSnapshot,
        registry
    };

    const auto upgraded = service.GetThumbnail(request);
    ASSERT_EQ(upgraded.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_TRUE(upgraded.residentPreviewRequest)
        << "The upgraded request must recognize its still-live resident snapshot.";
    EXPECT_TRUE(service.HasQueuedVisibleResidentThumbnail())
        << "A same-priority resident upgrade must update the authoritative request and lane.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPrefabPreviewPrunesCompletedObsoletePreparationsBeforeCapacityCheck)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    ScopedServiceOverride meshManagerOverride(meshManager);
    ScopedServiceOverride materialManagerOverride(materialManager);
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("71717171-7171-4171-8171-717171717171"));
    request.sourceAssetPath = "Assets/Prefabs/Missing.prefab";
    request.subAssetKey = "prefab:Missing";
    request.artifactPath = "Library/Artifacts/missing-prefab";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:preparation-capacity";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";

    EditorThumbnailPreviewRenderer renderer(EnsureThumbnailPerformanceTestDriver());
    for (size_t index = 0u; index < 8u; ++index)
    {
        request.freshnessInputs = {{"artifact", "missing:" + std::to_string(index)}};
        const auto pending = renderer.PumpResources(request);
        EXPECT_TRUE(pending.resourcesPending) << pending.diagnostic;
    }

    request.freshnessInputs = {{"artifact", "missing:ninth"}};
    EditorThumbnailPreviewResourcePumpResult ninth;
    for (size_t attempt = 0u; attempt < 256u; ++attempt)
    {
        ninth = renderer.PumpResources(request);
        if (ninth.diagnostic != "thumbnail-gpu-preview-resources-pending:prefab-prepare-capacity=1")
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(ninth.resourcesPending) << ninth.diagnostic;
    EXPECT_NE(ninth.diagnostic, "thumbnail-gpu-preview-resources-pending:prefab-prepare-capacity=1");
    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPrefabPreviewPumpDefersUntilMeshesAndMaterialsAreReady)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Serialize;

    const auto root = MakeThumbnailPerformanceRoot();
    auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto assetId = prefab.assetId;
    const auto meshAId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("50505050-5050-4050-8050-505050505050"));
    const auto materialAId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("60606060-6060-4060-8060-606060606060"));
    const auto meshBId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("51515151-5151-4151-8151-515151515151"));
    const auto materialBId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("61616161-6161-4161-8161-616161616161"));
    const auto prefabArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("c001000000000000000000000000000000000000000000000000000000000001");
    const auto meshAArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("c002000000000000000000000000000000000000000000000000000000000002");
    const auto materialAArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("c003000000000000000000000000000000000000000000000000000000000003");
    const auto meshBArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("c004000000000000000000000000000000000000000000000000000000000004");
    const auto materialBArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("c005000000000000000000000000000000000000000000000000000000000005");

    prefab.resolvedAssets[0].artifactPath = meshAArtifactPath;
    prefab.resolvedAssets[1].artifactPath = materialAArtifactPath;

    const auto secondGameObjectId = MakeObjectId("11111111-1111-4111-8111-111111111111");
    const auto secondMeshFilterId = MakeObjectId("12121212-1212-4121-8121-121212121212");
    const auto secondMeshRendererId = MakeObjectId("13131313-1313-4131-8131-131313131313");
    for (auto& object : prefab.graph.objects)
    {
        if (object.id != prefab.graph.root)
            continue;

        object.properties.push_back({
            "children",
            PropertyValue::Array({
                PropertyValue::OwnedReference(secondGameObjectId)
            })
        });
        break;
    }
    prefab.graph.objects.push_back(ObjectRecord{
        secondGameObjectId,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
        "PreviewSecond",
        "PreviewSecond",
        ObjectRecordState::Alive,
        {
            {
                "components",
                PropertyValue::Array({
                    PropertyValue::OwnedReference(secondMeshFilterId),
                    PropertyValue::OwnedReference(secondMeshRendererId)
                })
            },
            {
                "parent",
                PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(
                    MakeLocalIdentifierInFile(prefab.graph.root)))
            },
            MakePreviewTransformProperty(1.0, 2.0, 3.0, 1.0, 1.0, 1.0)
        },
        MakeLocalIdentifierInFile(secondGameObjectId)});
    prefab.graph.objects.push_back(ObjectRecord{
        secondMeshFilterId,
        NLS_TYPEOF(NLS::Engine::Components::MeshFilter).GetName(),
        "MeshFilter",
        "PreviewSecond/MeshFilter",
        ObjectRecordState::Alive,
        {
            {
                "mesh",
                PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(meshBId.GetGuid()),
                    1,
                    "mesh:HeroB"))
            }
        },
        MakeLocalIdentifierInFile(secondMeshFilterId)});
    prefab.graph.objects.push_back(ObjectRecord{
        secondMeshRendererId,
        NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName(),
        "MeshRenderer",
        "PreviewSecond/MeshRenderer",
        ObjectRecordState::Alive,
        {
            {
                "materials",
                PropertyValue::Array({
                    PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                        NLS::Engine::Serialize::AssetId(materialBId.GetGuid()),
                        1,
                        "material:HeroB"))
                })
            }
        },
        MakeLocalIdentifierInFile(secondMeshRendererId)});
    prefab.resolvedAssets.push_back({
        meshBId,
        "Mesh",
        "mesh:HeroB",
        meshBArtifactPath
    });
    prefab.resolvedAssets.push_back({
        materialBId,
        "Material",
        "material:HeroB",
        materialBArtifactPath
    });

    const auto prefabPayload = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        root / prefabArtifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteBinaryFile(root / meshAArtifactPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteBinaryFile(root / meshBArtifactPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteNativeArtifactTextFile(
        root / materialAArtifactPath,
        ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/AsyncMaterial.shader\n"
        "surfaceMode=Opaque\n");
    WriteNativeArtifactTextFile(
        root / materialBArtifactPath,
        ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/AsyncMaterial.shader\n"
        "surfaceMode=Opaque\n");
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = assetId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        assetId,
        "prefab:Hero",
        ArtifactType::Prefab,
        "native-prefab",
        prefabArtifactPath,
        "prefab-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        meshAId,
        "mesh:Hero",
        ArtifactType::Mesh,
        "mesh",
        meshAArtifactPath,
        "mesh-a-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        materialAId,
        "material:Hero",
        ArtifactType::Material,
        "native-material",
        materialAArtifactPath,
        "material-a-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        meshBId,
        "mesh:HeroB",
        ArtifactType::Mesh,
        "mesh",
        meshBArtifactPath,
        "mesh-b-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        materialBId,
        "material:HeroB",
        ArtifactType::Material,
        "native-material",
        materialBArtifactPath,
        "material-b-hash"));
    WriteThumbnailPerformanceArtifactDatabase(root, manifest);

    CountingMeshManager meshManager;
    CountingMaterialManager materialManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerOverride(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerOverride(materialManager);
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    auto loadedPrefab = database.LoadPrefabArtifactByAssetId(assetId, "prefab:Hero");
    ASSERT_TRUE(loadedPrefab.has_value());
    const auto loadedSnapshot = BuildPreviewRenderableSnapshot(*loadedPrefab);
    ASSERT_EQ(loadedSnapshot.drawItems.size(), 2u);
    ASSERT_EQ(loadedSnapshot.drawItems[0].materialPaths.size(), 1u);
    ASSERT_EQ(loadedSnapshot.drawItems[1].materialPaths.size(), 1u);

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.subAssetKey = "prefab:Hero";
    request.artifactPath = prefabArtifactPath;
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:batch-resource-request";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {{"artifact", "batch-resource-request:v1"}};

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    auto& driver = EnsureThumbnailPerformanceTestDriver();
    EditorThumbnailPreviewRenderer renderer(driver);
    const auto readyMeshArtifact = TriangleMeshArtifact();
    auto makeReadyMesh = [&readyMeshArtifact]()
    {
        return new NLS::Render::Resources::Mesh(
            readyMeshArtifact.vertices,
            readyMeshArtifact.indices,
            readyMeshArtifact.materialIndex,
            NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
            readyMeshArtifact.boundingSphere);
    };
    // The scene may register the same canonical artifact under its portable
    // Library path while the thumbnail snapshot carries the resolved absolute
    // path. Both must be treated as the same resident resource.
    meshManager.RegisterResource(meshAArtifactPath, makeReadyMesh());
    materialManager.RegisterResource(
        materialAArtifactPath,
        new NLS::Render::Resources::Material());

    EditorThumbnailPreviewResourcePumpResult mixedPump;
    for (size_t attempt = 0u;
        attempt < 128u && (meshManager.asyncRequestCount == 0u || materialManager.asyncRequestCount < 1u);
        ++attempt)
    {
        mixedPump = renderer.PumpResources(request);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(mixedPump.supported);
    EXPECT_TRUE(mixedPump.resourcesPending);
    ExpectResourcesPendingDiagnostic(mixedPump.diagnostic);
    EXPECT_EQ(meshManager.asyncRequestCount, 1u);
    EXPECT_TRUE(ContainsPathWithSuffix(
        meshManager.asyncRequestPaths,
        std::filesystem::path(meshBArtifactPath).filename().generic_string()));
    EXPECT_EQ(materialManager.asyncRequestCount, 1u);
    EXPECT_TRUE(ContainsPathWithSuffix(
        materialManager.asyncRequestPaths,
        std::filesystem::path(materialBArtifactPath).filename().generic_string()));

    const auto mixedRender = renderer.Render(request);
    EXPECT_TRUE(mixedRender.rgbaPixels.empty());
    ExpectResourcesPendingDiagnostic(mixedRender.diagnostic);
    EXPECT_EQ(mixedRender.rawVisibleDrawCount, 0u);
    EXPECT_EQ(mixedRender.submittedSceneDrawCount, 0u);

    meshManager.RegisterResource((root / meshBArtifactPath).generic_string(), makeReadyMesh());
    const auto completePump = renderer.PumpResources(request);
    EXPECT_TRUE(completePump.supported);
    EXPECT_TRUE(completePump.resourcesPending);
    EXPECT_NE(completePump.diagnostic.find("material=1"), std::string::npos)
        << "Ready meshes and resident equivalent-path materials must not bypass the remaining cold dependency.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPrefabPreviewRendersReadyPrefabToVisiblePixels)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Serialize;

    const auto root = MakeThumbnailPerformanceRoot();
    auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto assetId = prefab.assetId;
    const auto prefabArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("d101000000000000000000000000000000000000000000000000000000000001");
    const auto meshArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("d102000000000000000000000000000000000000000000000000000000000002");
    const auto materialArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("d103000000000000000000000000000000000000000000000000000000000003");
    const auto meshId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("50505050-5050-4050-8050-505050505050"));
    const auto materialId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("60606060-6060-4060-8060-606060606060"));
    const auto meshBId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("51515151-5151-4151-8151-515151515151"));
    const auto materialBId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("61616161-6161-4161-8161-616161616161"));
    const auto meshBArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("d104000000000000000000000000000000000000000000000000000000000004");
    const auto materialBArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("d105000000000000000000000000000000000000000000000000000000000005");
    prefab.resolvedAssets[0].artifactPath = meshArtifactPath;
    prefab.resolvedAssets[1].artifactPath = materialArtifactPath;

    const auto secondGameObjectId = MakeObjectId("11111111-1111-4111-8111-111111111111");
    const auto secondMeshFilterId = MakeObjectId("12121212-1212-4121-8121-121212121212");
    const auto secondMeshRendererId = MakeObjectId("13131313-1313-4131-8131-131313131313");
    for (auto& object : prefab.graph.objects)
    {
        if (object.id != prefab.graph.root)
            continue;
        object.properties.push_back({
            "children",
            PropertyValue::Array({PropertyValue::OwnedReference(secondGameObjectId)})});
        break;
    }
    prefab.graph.objects.push_back(ObjectRecord{
        secondGameObjectId,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
        "PreviewSecond",
        "PreviewSecond",
        ObjectRecordState::Alive,
        {
            {
                "components",
                PropertyValue::Array({
                    PropertyValue::OwnedReference(secondMeshFilterId),
                    PropertyValue::OwnedReference(secondMeshRendererId)})
            },
            {
                "parent",
                PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(
                    MakeLocalIdentifierInFile(prefab.graph.root)))
            },
            MakePreviewTransformProperty(-4.0, 0.0, 0.0, 1.0, 1.0, 1.0)
        },
        MakeLocalIdentifierInFile(secondGameObjectId)});
    prefab.graph.objects.push_back(ObjectRecord{
        secondMeshFilterId,
        NLS_TYPEOF(NLS::Engine::Components::MeshFilter).GetName(),
        "MeshFilter",
        "PreviewSecond/MeshFilter",
        ObjectRecordState::Alive,
        {
            {
                "mesh",
                PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(meshBId.GetGuid()),
                    1,
                    "mesh:HeroB"))
            }
        },
        MakeLocalIdentifierInFile(secondMeshFilterId)});
    prefab.graph.objects.push_back(ObjectRecord{
        secondMeshRendererId,
        NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName(),
        "MeshRenderer",
        "PreviewSecond/MeshRenderer",
        ObjectRecordState::Alive,
        {
            {
                "materials",
                PropertyValue::Array({PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(materialBId.GetGuid()),
                    1,
                    "material:HeroB"))})
            }
        },
        MakeLocalIdentifierInFile(secondMeshRendererId)});
    prefab.resolvedAssets.push_back({meshBId, "Mesh", "mesh:HeroB", meshBArtifactPath});
    prefab.resolvedAssets.push_back({materialBId, "Material", "material:HeroB", materialBArtifactPath});
    const auto prefabPayload = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);

    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        root / prefabArtifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteBinaryFile(root / meshArtifactPath, NLS::Render::Assets::SerializeMeshArtifact(LitTriangleMeshArtifact()));
    WriteBinaryFile(root / meshBArtifactPath, NLS::Render::Assets::SerializeMeshArtifact(LitTriangleMeshArtifact()));
    WriteNativeArtifactTextFile(
        root / materialArtifactPath,
        ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");
    WriteNativeArtifactTextFile(
        root / materialBArtifactPath,
        ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = assetId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        assetId,
        "prefab:Hero",
        ArtifactType::Prefab,
        "native-prefab",
        prefabArtifactPath,
        "prefab-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        meshId,
        "mesh:Hero",
        ArtifactType::Mesh,
        "mesh",
        meshArtifactPath,
        "mesh-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        materialId,
        "material:Hero",
        ArtifactType::Material,
        "native-material",
        materialArtifactPath,
        "material-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        meshBId,
        "mesh:HeroB",
        ArtifactType::Mesh,
        "mesh",
        meshBArtifactPath,
        "mesh-b-hash"));
    manifest.subAssets.push_back(MakeThumbnailPerformanceImportedArtifact(
        materialBId,
        "material:HeroB",
        ArtifactType::Material,
        "native-material",
        materialBArtifactPath,
        "material-b-hash"));
    WriteThumbnailPerformanceArtifactDatabase(root, manifest);

    CountingMeshManager meshManager;
    CountingMaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerOverride(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerOverride(materialManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::ShaderManager> shaderManagerOverride(shaderManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::TextureManager> textureManagerOverride(textureManager);
    const auto repositoryEngineAssetsRoot =
        std::filesystem::current_path() / "App" / "Assets" / "Engine";
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", repositoryEngineAssetsRoot);
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    auto& driver = EnsureThumbnailPerformanceGpuTestDriver();
    if (driver.GetActiveGraphicsBackend() == NLS::Render::Settings::EGraphicsBackend::NONE)
    {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "No explicit GPU backend is available for prefab thumbnail render verification.";
    }

    const auto meshArtifact = LitTriangleMeshArtifact();
    auto makeMesh = [&meshArtifact]()
    {
        return new NLS::Render::Resources::Mesh(
            meshArtifact.vertices,
            meshArtifact.indices,
            meshArtifact.materialIndex,
            NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
            meshArtifact.boundingSphere);
    };
    meshManager.RegisterResource((root / meshArtifactPath).generic_string(), makeMesh());
    auto* shader = shaderManager.GetResource(":Shaders/StandardPBR.hlsl", true);
    ASSERT_NE(shader, nullptr);
    shader->SetImportedShaderLabPassForTesting(
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
        "shader:standardpbr/forward#thumbnail-prefab",
        "Forward",
        {});
    auto* whiteTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(255u, 255u, 255u, 255u);
    auto* blackTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(0u, 0u, 0u, 255u);
    ASSERT_NE(whiteTexture, nullptr);
    ASSERT_NE(blackTexture, nullptr);
    textureManager.RegisterResource(":test/thumbnail-prefab-white", whiteTexture);
    textureManager.RegisterResource(":test/thumbnail-prefab-black", blackTexture);
    auto makeMaterial = [shader, whiteTexture, blackTexture](const NLS::Maths::Vector4& color)
    {
        auto* material = new NLS::Render::Resources::Material(shader);
        material->SetShaderLabSourcePath("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader");
        material->RegisterShaderLabPassShader(shader);
        material->SetRawParameter("u_Albedo", color);
        material->SetRawParameter("u_Metallic", 0.0f);
        material->SetRawParameter("u_Roughness", 0.72f);
        material->SetRawParameter("u_AmbientOcclusion", 1.0f);
        material->SetRawParameter("u_EnableNormalMapping", 0.0f);
        material->SetRawParameter("u_Emissive", color);
        material->SetRawParameter("u_Specular", NLS::Maths::Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        material->SetRawParameter("u_MetallicMapChannel", NLS::Maths::Vector4(0.0f, 0.0f, 1.0f, 0.0f));
        material->SetRawParameter("u_RoughnessMapChannel", NLS::Maths::Vector4(0.0f, 0.0f, 1.0f, 0.0f));
        material->SetRawParameter("u_AlbedoMap", whiteTexture);
        material->SetRawParameter("u_MetallicMap", whiteTexture);
        material->SetRawParameter("u_RoughnessMap", whiteTexture);
        material->SetRawParameter("u_AmbientOcclusionMap", whiteTexture);
        material->SetRawParameter("u_NormalMap", whiteTexture);
        material->SetRawParameter("u_OpacityMap", whiteTexture);
        material->SetRawParameter("u_EmissiveMap", whiteTexture);
        material->SetRawParameter("u_SpecularMap", blackTexture);
        material->SetBackfaceCulling(false);
        material->SetFrontfaceCulling(false);
        material->SetDepthTest(true);
        material->SetDepthWriting(true);
        material->SetColorWriting(true);
        return material;
    };
    materialManager.RegisterResource(
        (root / materialArtifactPath).generic_string(),
        makeMaterial(NLS::Maths::Vector4(0.72f, 0.74f, 0.78f, 1.0f)));

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.subAssetKey = "prefab:Hero";
    request.artifactPath = prefabArtifactPath;
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:ready-prefab-visible";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {{"artifact", "ready-prefab-visible:v1"}};

    EditorThumbnailPreviewRenderer renderer(driver);
    EditorThumbnailPreviewResult waiting;
    for (size_t attempt = 0u; attempt < 2048u && meshManager.asyncRequestCount == 0u; ++attempt)
    {
        waiting = renderer.Render(request);
        if (waiting.rgbaPixels.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(waiting.rgbaPixels.empty());
    EXPECT_GT(meshManager.asyncRequestCount, 0u)
        << "The second Mesh must be observed as a missing resource before the completion retry.";

    meshManager.RegisterResource((root / meshBArtifactPath).generic_string(), makeMesh());
    materialManager.RegisterResource(
        (root / materialBArtifactPath).generic_string(),
        makeMaterial(NLS::Maths::Vector4(0.20f, 0.70f, 0.95f, 1.0f)));

    EditorThumbnailPreviewResult rendered;
    bool publishedGpuTextureBeforeReadback = false;
    for (size_t attempt = 0u; attempt < 2048u && rendered.rgbaPixels.empty(); ++attempt)
    {
        rendered = renderer.Render(request);
        publishedGpuTextureBeforeReadback =
            publishedGpuTextureBeforeReadback || rendered.gpuTexture.IsValid();
        if (rendered.rgbaPixels.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#if defined(NLS_ENABLE_TEST_HOOKS)
    const auto drawStats = GetLastThumbnailPreviewRenderStatsForTesting();
#endif

    ASSERT_FALSE(rendered.rgbaPixels.empty()) << rendered.diagnostic;
    EXPECT_TRUE(publishedGpuTextureBeforeReadback)
        << "The DX12 preview texture should be publishable before readback/PNG persistence completes.";
    ASSERT_EQ(rendered.width, 64u);
    ASSERT_EQ(rendered.height, 64u);
    EXPECT_EQ(rendered.expectedSceneDrawCount, 2u);
    EXPECT_EQ(rendered.rawVisibleDrawCount, 2u);
    EXPECT_EQ(rendered.submittedSceneDrawCount, 2u);
    const auto reuseStatsAfterFirstRender = renderer.GetReuseStats();
    EXPECT_GE(reuseStatsAfterFirstRender.previewSceneUseCount, 1u);
    EXPECT_EQ(reuseStatsAfterFirstRender.renderTargetAllocationCount, 1u);
    EXPECT_EQ(reuseStatsAfterFirstRender.renderTargetPoolSize, 1u);

    rendered.gpuTexture = {};
    EditorThumbnailPreviewResult repeatedRender;
    for (size_t attempt = 0u; attempt < 2048u && repeatedRender.rgbaPixels.empty(); ++attempt)
    {
        repeatedRender = renderer.Render(request);
        if (repeatedRender.rgbaPixels.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_FALSE(repeatedRender.rgbaPixels.empty()) << repeatedRender.diagnostic;
    const auto reuseStatsAfterRepeatedRender = renderer.GetReuseStats();
    EXPECT_GE(reuseStatsAfterRepeatedRender.previewSceneUseCount, 2u);
    EXPECT_EQ(reuseStatsAfterRepeatedRender.renderTargetAllocationCount, 1u);
    EXPECT_GT(
        reuseStatsAfterRepeatedRender.renderTargetReuseCount,
        reuseStatsAfterFirstRender.renderTargetReuseCount);
    EXPECT_EQ(reuseStatsAfterRepeatedRender.renderTargetPoolSize, 1u);

    if (const char* proofPath = std::getenv("NLS_THUMBNAIL_PROOF_PATH");
        proofPath != nullptr && proofPath[0] != '\0')
    {
        std::filesystem::create_directories(std::filesystem::path(proofPath).parent_path());
        NLS::Image proofImage(
            static_cast<int>(rendered.width),
            static_cast<int>(rendered.height),
            4);
        proofImage.SetData(rendered.rgbaPixels.data());
        proofImage.Save(proofPath);
    }
    size_t litPixelCount = 0u;
    size_t visiblePixelCount = 0u;
    size_t transparentBackgroundPixelCount = 0u;
    uint8_t minAlpha = 255u;
    uint8_t maxAlpha = 0u;
    uint8_t maxLuma = 0u;
    for (size_t pixel = 0u; pixel + 3u < rendered.rgbaPixels.size(); pixel += 4u)
    {
        const uint8_t alpha = rendered.rgbaPixels[pixel + 3u];
        const uint8_t red = rendered.rgbaPixels[pixel + 0u];
        const uint8_t green = rendered.rgbaPixels[pixel + 1u];
        const uint8_t blue = rendered.rgbaPixels[pixel + 2u];
        const auto luma = static_cast<uint8_t>(
            (static_cast<uint16_t>(red) * 77u +
                static_cast<uint16_t>(green) * 150u +
                static_cast<uint16_t>(blue) * 29u) >> 8u);
        minAlpha = (std::min)(minAlpha, alpha);
        maxAlpha = (std::max)(maxAlpha, alpha);
        maxLuma = (std::max)(maxLuma, luma);
        if (alpha <= 8u && luma <= 8u)
            ++transparentBackgroundPixelCount;
        if (alpha > 8u)
            ++visiblePixelCount;
        if (alpha > 8u && luma > 8u)
            ++litPixelCount;
    }
    EXPECT_GT(litPixelCount, 8u)
        << "Ready prefab GPU previews must draw visible pixels instead of returning a black/clear readback. "
        << "visiblePixelCount=" << visiblePixelCount
        << " maxAlpha=" << static_cast<int>(maxAlpha)
        << " maxLuma=" << static_cast<int>(maxLuma)
        << " diagnostic=" << rendered.diagnostic
#if defined(NLS_ENABLE_TEST_HOOKS)
        << " rawVisibleDrawCount=" << drawStats.rawVisibleDrawCount
        << " submittedSceneDrawCount=" << drawStats.submittedSceneDrawCount
#endif
        ;
    size_t leftVisiblePixelCount = 0u;
    size_t rightVisiblePixelCount = 0u;
    for (uint32_t y = 0u; y < rendered.height; ++y)
    {
        for (uint32_t x = 0u; x < rendered.width; ++x)
        {
            const size_t pixel = (static_cast<size_t>(y) * rendered.width + x) * 4u;
            if (pixel + 3u >= rendered.rgbaPixels.size() || rendered.rgbaPixels[pixel + 3u] <= 8u)
                continue;
            if (x < rendered.width / 2u)
                ++leftVisiblePixelCount;
            else
                ++rightVisiblePixelCount;
        }
    }
    EXPECT_GT(leftVisiblePixelCount, 4u)
        << "The first spatially separated Mesh must remain visible in the final Prefab thumbnail.";
    EXPECT_GT(rightVisiblePixelCount, 4u)
        << "The second spatially separated Mesh must remain visible in the final Prefab thumbnail.";
    EXPECT_GT(transparentBackgroundPixelCount, 512u)
        << "Prefab GPU previews must preserve a transparent offscreen background instead of clearing alpha opaque. "
        << "transparentBackgroundPixelCount=" << transparentBackgroundPixelCount
        << " minAlpha=" << static_cast<int>(minAlpha)
        << " maxAlpha=" << static_cast<int>(maxAlpha)
        << " maxLuma=" << static_cast<int>(maxLuma)
        << " diagnostic=" << rendered.diagnostic;

    std::filesystem::remove_all(root);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(AssetThumbnailBehaviorTests, StablePreviewMaterialPreservesShaderLabSourcePath)
{
    auto* forwardShader = NLS::Render::Resources::Shader::CreateForTesting(":test/standard-pbr-forward");
    ASSERT_NE(forwardShader, nullptr);
    forwardShader->SetImportedShaderLabPassForTesting(
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
        "shader:standardpbr/forward#test",
        "Forward",
        {});

    {
        NLS::Render::Resources::Material source;
        source.RegisterShaderLabPassShader(forwardShader);
        source.SetShaderLabSourcePath("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader");
        source.SetRawParameter("_BaseColor", NLS::Maths::Vector4(0.25f, 0.5f, 0.75f, 1.0f));
        source.EnableKeyword("_NORMALMAP");
        source.EnableKeyword("_ALPHATEST_ON");

        auto preview = NLS::Editor::Assets::CreateStablePreviewMaterialForTesting(source);

        ASSERT_NE(preview, nullptr);
        EXPECT_EQ(
            preview->GetShaderLabSourcePath(),
            "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader");
        EXPECT_TRUE(preview->HasExplicitShaderLabSourcePath());
        EXPECT_EQ(preview->ResolveShaderForLightMode("Forward"), forwardShader);
        EXPECT_TRUE(preview->IsKeywordEnabled("_NORMALMAP"));
        EXPECT_TRUE(preview->IsKeywordEnabled("_ALPHATEST_ON"));
        EXPECT_FALSE(preview->HasBackfaceCulling());
        EXPECT_FALSE(preview->HasFrontfaceCulling())
            << "Thumbnail-only material copies must reveal interior-authored geometry from an exterior upper-oblique camera.";
        const auto* copiedBaseColor = preview->GetParameterBlock().TryGet("_BaseColor");
        ASSERT_NE(copiedBaseColor, nullptr);
        const auto* copiedBaseColorValue = std::any_cast<NLS::Maths::Vector4>(copiedBaseColor);
        ASSERT_NE(copiedBaseColorValue, nullptr);
        EXPECT_FLOAT_EQ(copiedBaseColorValue->x, 0.25f);
        EXPECT_FLOAT_EQ(copiedBaseColorValue->y, 0.5f);
        EXPECT_FLOAT_EQ(copiedBaseColorValue->z, 0.75f);
        EXPECT_FLOAT_EQ(copiedBaseColorValue->w, 1.0f);
    }
    NLS::Render::Resources::Shader::DestroyForTesting(forwardShader);
}

TEST(AssetThumbnailBehaviorTests, StablePreviewMaterialRejectsMismatchedShaderLabFallback)
{
    auto* mismatchedShader = NLS::Render::Resources::Shader::CreateForTesting(":test/mismatched-forward");
    ASSERT_NE(mismatchedShader, nullptr);
    mismatchedShader->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Other.shader",
        "shader:other/forward#test",
        "Forward",
        {});

    {
        NLS::Render::Resources::Material source(mismatchedShader);
        source.SetShaderLabSourcePath("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader");

        auto preview = NLS::Editor::Assets::CreateStablePreviewMaterialForTesting(source);

        ASSERT_NE(preview, nullptr);
        EXPECT_EQ(
            preview->GetShaderLabSourcePath(),
            "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader");
        EXPECT_TRUE(preview->HasExplicitShaderLabSourcePath());
        EXPECT_EQ(preview->GetShader(), mismatchedShader);
        EXPECT_EQ(preview->ResolveShaderForLightMode("Forward"), nullptr);
    }
    NLS::Render::Resources::Shader::DestroyForTesting(mismatchedShader);
}
#endif

TEST(AssetThumbnailBehaviorTests, GpuPreviewResourcePumpBudgetsAdvanceSeveralBoundedSteps)
{
    EXPECT_GE(NLS::Editor::Assets::GetThumbnailPreviewMeshPumpBudgetForTesting(), 4u);
    EXPECT_EQ(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabMeshRequestStartBudgetForTesting(),
        4u)
        << "Large prefab mesh requests must advance in a bounded batch so a large prefab does not remain pending for the full deadline.";
    EXPECT_EQ(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabMeshPumpBudgetForTesting(),
        4u)
        << "Large prefab mesh completions must remain bounded while allowing the resource continuation to make progress.";
    EXPECT_EQ(NLS::Editor::Assets::GetThumbnailPreviewMaterialPumpBudgetForTesting(), 4u)
        << "Material artifact promotion must advance in a bounded batch so visible prefab previews do not stall behind one completion per frame.";
    EXPECT_GE(NLS::Editor::Assets::GetThumbnailPreviewTexturePumpBudgetForTesting(), 4u);
    EXPECT_EQ(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabTexturePumpBudgetForTesting(),
        4u)
        << "Large prefab texture completions remain bounded by the one millisecond resource-pump deadline.";
    EXPECT_GE(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabResourceInspectionBudgetForTesting(),
        32u)
        << "Large visible prefabs must discover missing mesh requests in wide cheap batches; a four-item batch combined with resource-pending cooldown takes minutes before rendering can begin.";
    EXPECT_GT(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabResourcePumpTimeBudgetMicrosForTesting(),
        0u)
        << "Count limits alone cannot protect the UI when one artifact promotion is unexpectedly expensive.";
    EXPECT_FALSE(NLS::Editor::Assets::ShouldYieldPrefabMeshDependencyInspectionForTesting(false, 0u, 1u));
    EXPECT_TRUE(NLS::Editor::Assets::ShouldYieldPrefabMeshDependencyInspectionForTesting(false, 1u, 1u));
    EXPECT_FALSE(NLS::Editor::Assets::ShouldYieldPrefabMeshDependencyInspectionForTesting(true, 1u, 1u));
    EXPECT_GT(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabSceneAssemblyBudgetForTesting(),
        0u);
    EXPECT_LT(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabSceneAssemblyBudgetForTesting(),
        NLS::Editor::Assets::GetThumbnailPreviewPrefabDrawItemCapacityForTesting())
        << "Prefab preview scene objects must be assembled across frames so a complex thumbnail cannot monopolize the UI thread.";
}

TEST(AssetThumbnailBehaviorTests, ThumbnailSchedulerSmoothsCompletedWorkWithEwma)
{
    using namespace NLS::Editor::Assets;

    AssetThumbnailRenderSchedulerConfig config;
    config.idleInitialBudgetMicroseconds = 1000u;
    config.idleMinimumBudgetMicroseconds = 250u;
    config.idleMaximumBudgetMicroseconds = 2000u;
    config.budgetRecoveryStepMicroseconds = 125u;
    AssetThumbnailRenderScheduler scheduler(config);

    scheduler.BeginFrame(1u, false);
    ASSERT_TRUE(scheduler.TryBeginCompletedResult());
    scheduler.FinishActiveWork(800u);

    scheduler.BeginFrame(2u, false);
    EXPECT_EQ(scheduler.GetConsumedEwmaMicroseconds(), 800u);
    ASSERT_TRUE(scheduler.TryBeginCompletedResult());
    scheduler.FinishActiveWork(100u);

    scheduler.BeginFrame(3u, false);
    EXPECT_EQ(scheduler.GetConsumedEwmaMicroseconds(), 788u);
    EXPECT_EQ(
        scheduler.GetFrameStats().budgetMicroseconds,
        config.idleInitialBudgetMicroseconds)
        << "A single expensive frame must not immediately collapse the idle budget.";
}

TEST(AssetThumbnailBehaviorTests, ResidentPreviewAdvancesWhileSceneResourcesArePending)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserHeavyGpuThumbnailPumpInput residentInput;
    residentInput.allowHeavyGpuPreview = false;
    residentInput.hasQueuedWork = true;
    residentInput.hasPreviewRenderer = true;
    residentInput.sceneLoadRendererResourcesPending = true;
    residentInput.hasQueuedVisibleResidentPreview = true;
    residentInput.nowSeconds = 1.0;
    residentInput.deferredUntilSeconds = 100.0;

    AssetThumbnailRenderScheduler residentScheduler;
    residentScheduler.BeginFrame(1u, false);
    EXPECT_TRUE(residentScheduler.TryBeginHeavyGpuPreview(residentInput))
        << "A visible resident snapshot must reach the renderer so ready resources can render immediately; "
           "the renderer uses join-only resource resolution while the scene gate is active.";
    residentScheduler.FinishActiveWork(100u);

    residentInput.hasQueuedVisibleResidentPreview = false;
    residentInput.deferredUntilSeconds = 0.0;
    residentScheduler.BeginFrame(2u, false);
    EXPECT_FALSE(residentScheduler.TryBeginHeavyGpuPreview(residentInput))
        << "A non-resident preview must not start a second resource path while scene resources are pending.";
    EXPECT_EQ(
        residentScheduler.GetLastRejection(),
        AssetThumbnailRenderScheduleRejection::SceneLoadRendererResourcesPending);
}

TEST(AssetThumbnailBehaviorTests, ResourceContinuationAdvancesWhenHeavyStartsAreDisabled)
{
    using namespace NLS::Editor::Assets;

    AssetBrowserHeavyGpuThumbnailPumpInput input;
    input.allowHeavyGpuPreview = false;
    input.hasQueuedWork = true;
    input.hasQueuedResourceContinuation = true;
    input.hasPreviewRenderer = true;
    input.sceneLoadRendererResourcesPending = true;

    AssetThumbnailRenderScheduler scheduler;
    scheduler.BeginFrame(1u, false);
    EXPECT_TRUE(scheduler.TryBeginHeavyGpuPreview(input))
        << "Existing resource continuations must progress even while new heavy previews are disabled.";
    scheduler.FinishActiveWork(100u);
}

TEST(AssetThumbnailBehaviorTests, HeavyGpuPreviewEventuallyAdvancesAcrossOverTargetFrames)
{
    using namespace NLS::Editor::Assets;

    AssetThumbnailRenderSchedulerConfig config;
    config.oversizedWorkRetryFrameCount = 3u;
    AssetThumbnailRenderScheduler scheduler(config);

    AssetBrowserHeavyGpuThumbnailPumpInput input;
    input.allowHeavyGpuPreview = true;
    input.hasQueuedWork = true;
    input.hasPreviewRenderer = true;

    for (uint64_t frameSerial = 1u;
        frameSerial < config.oversizedWorkRetryFrameCount;
        ++frameSerial)
    {
        scheduler.BeginFrame(frameSerial, false, 0u, true);
        EXPECT_FALSE(scheduler.TryBeginHeavyGpuPreview(input));
        EXPECT_EQ(
            scheduler.GetLastRejection(),
            AssetThumbnailRenderScheduleRejection::PreviousFrameOverTarget);
    }

    scheduler.BeginFrame(config.oversizedWorkRetryFrameCount, false, 0u, true);
    EXPECT_TRUE(scheduler.TryBeginHeavyGpuPreview(input))
        << "A continuously over-target scene must not permanently starve a new heavy preview.";
    scheduler.FinishActiveWork(100u);
}

TEST(AssetThumbnailBehaviorTests, PrefabDrawPrewarmStateRestoresOnlyForSameResourcePlan)
{
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(ShouldRestorePrefabPreviewDrawPrewarmStateForTesting(
        true,
        17u,
        17u,
        112u,
        405u,
        false));
    EXPECT_TRUE(ShouldRestorePrefabPreviewDrawPrewarmStateForTesting(
        true,
        17u,
        17u,
        405u,
        405u,
        true));
    EXPECT_FALSE(ShouldRestorePrefabPreviewDrawPrewarmStateForTesting(
        true,
        17u,
        18u,
        112u,
        405u,
        false))
        << "A changed resource plan must restart draw prewarming from a fresh cursor.";
    EXPECT_FALSE(ShouldRestorePrefabPreviewDrawPrewarmStateForTesting(
        false,
        17u,
        17u,
        112u,
        405u,
        false))
        << "An expired prepared snapshot must not restore a stale draw cursor.";
}

TEST(AssetThumbnailBehaviorTests, ResourcePumpGivesEachDependencyPhaseOneAttempt)
{
    using namespace NLS::Editor::Assets;

    EXPECT_FALSE(ShouldContinuePrefabPreviewResourceInspectionForTesting(0u, 8u, true));
    EXPECT_FALSE(ShouldContinuePrefabPreviewResourceInspectionForTesting(1u, 8u, true));
    EXPECT_TRUE(ShouldContinuePrefabPreviewResourceInspectionForTesting(0u, 8u, false));
    EXPECT_TRUE(ShouldContinuePrefabPreviewResourceInspectionForTesting(1u, 8u, false));
    EXPECT_FALSE(ShouldContinuePrefabPreviewResourceInspectionForTesting(0u, 0u, true));
}

TEST(AssetThumbnailBehaviorTests, CompletedResourcePhaseRenewsNextPhaseBudget)
{
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(ShouldResetPrefabPreviewPhaseDeadlineForTesting(0u, 0u, 0u));
    EXPECT_FALSE(ShouldResetPrefabPreviewPhaseDeadlineForTesting(1u, 0u, 0u));
    EXPECT_TRUE(ShouldResetPrefabPreviewPhaseDeadlineForTesting(0u, 1u, 0u))
        << "A stale accepted-request marker must not starve the next dependency phase.";
    EXPECT_FALSE(ShouldResetPrefabPreviewPhaseDeadlineForTesting(0u, 0u, 1u));
}

TEST(AssetThumbnailBehaviorTests, ExplicitPrefabMeshPumpDoesNotRepeatGlobalCompletionPump)
{
    using namespace NLS::Editor::Assets;

    EXPECT_FALSE(ShouldPumpPrefabRuntimeUploadRetirementForTesting(4u, 4u))
        << "Explicit paths already retire their matching runtime uploads.";
    EXPECT_FALSE(ShouldPumpPrefabRuntimeUploadRetirementForTesting(1u, 0u));
    EXPECT_TRUE(ShouldPumpPrefabRuntimeUploadRetirementForTesting(0u, 1u))
        << "A drained path cursor must still retire a delayed RHI completion.";
    EXPECT_FALSE(ShouldPumpPrefabRuntimeUploadRetirementForTesting(0u, 0u));
}

TEST(AssetThumbnailBehaviorTests, TextureInspectionRenewsBudgetAfterFixedSetupOnlyWithoutPendingPump)
{
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(ShouldRefreshPrefabPreviewTextureInspectionDeadlineAfterSetupForTesting(
        true,
        0u));
    EXPECT_FALSE(ShouldRefreshPrefabPreviewTextureInspectionDeadlineAfterSetupForTesting(
        true,
        1u)) << "An existing texture request must remain inside the original phase budget.";
    EXPECT_FALSE(ShouldRefreshPrefabPreviewTextureInspectionDeadlineAfterSetupForTesting(
        false,
        0u)) << "An unfinished material phase must not grant extra texture work.";
}

TEST(AssetThumbnailBehaviorTests, MaterialResourceContentionEventuallyUsesDuplicateSafeLookup)
{
    using namespace NLS::Editor::Assets;

    EXPECT_FALSE(ShouldWaitForPrefabPreviewMaterialResourceTableForTesting(7u, true, false));
    EXPECT_TRUE(ShouldWaitForPrefabPreviewMaterialResourceTableForTesting(8u, true, false))
        << "A cold material must not remain pending forever when the scene renderer owns the resource table each frame.";
    EXPECT_FALSE(ShouldWaitForPrefabPreviewMaterialResourceTableForTesting(8u, false, false))
        << "Join-only resident work must not create a duplicate material request.";
    EXPECT_FALSE(ShouldWaitForPrefabPreviewMaterialResourceTableForTesting(8u, true, true))
        << "Scene restoration retains ownership until its blocking resource gate clears.";
}

TEST(AssetThumbnailBehaviorTests, ActiveResourceWorkRefreshesUnchangedProgressToken)
{
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(ShouldRefreshGpuPreviewResourceProgressForTesting(17u, 17u, true));
    EXPECT_TRUE(ShouldRefreshGpuPreviewResourceProgressForTesting(17u, 18u, false));
    EXPECT_FALSE(ShouldRefreshGpuPreviewResourceProgressForTesting(17u, 17u, false))
        << "A genuinely stalled continuation must still remain eligible for the bounded timeout.";
}

TEST(AssetThumbnailBehaviorTests, LargeUnloadedPrefabDefersUntilResident)
{
    using namespace NLS::Editor::Assets;

    constexpr size_t kSourceDrawItemCount = 405u;
    EXPECT_TRUE(ShouldDeferLargePrefabPreviewUntilResidentForTesting(
        kSourceDrawItemCount,
        false));
}

TEST(AssetThumbnailBehaviorTests, MeshManagerPumpAsyncLoadsForPathsLeavesUnrelatedThumbnailRequestsPending)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async mesh request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    EnsureThumbnailPerformanceTestDriver();
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto targetPath = root / "Assets" / "target.nmesh";
    const auto unrelatedPath = root / "Assets" / "unrelated.nmesh";
    WriteBinaryFile(targetPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteBinaryFile(unrelatedPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    EXPECT_EQ(meshManager.RequestAsyncArtifact(targetPath.string()), nullptr);
    EXPECT_EQ(meshManager.RequestAsyncArtifact(unrelatedPath.string()), nullptr);

    for (size_t attempt = 0; attempt < 64u && MeshManager::GetPendingAsyncArtifactRequestCountForTesting() > 1u; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    meshManager.PumpAsyncLoadsForPaths({targetPath.string()}, 8u);

    EXPECT_TRUE(meshManager.IsResourceRegistered(targetPath.string()));
    EXPECT_FALSE(meshManager.IsResourceRegistered(unrelatedPath.string()));
    EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPending(unrelatedPath.string()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, PreparedMeshArtifactRequestUsesMemoryAndReleasesBytesAfterDecode)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async mesh request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    EnsureThumbnailPerformanceTestDriver();
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto missingCommittedPath = root / "Assets" / "prepared-import.nmesh";
    ASSERT_FALSE(std::filesystem::exists(missingCommittedPath));

    auto payload = std::make_shared<const std::vector<uint8_t>>(
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    std::weak_ptr<const std::vector<uint8_t>> payloadLifetime = payload;
    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    EXPECT_EQ(
        meshManager.RequestAsyncPreparedArtifactForPreview(
            missingCommittedPath.string(),
            payload),
        nullptr);
    payload.reset();
    EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPendingExactPath(missingCommittedPath.string()));
    ASSERT_TRUE(MeshManager::WaitForAsyncArtifactWorkersForTesting());
    EXPECT_TRUE(payloadLifetime.expired())
        << "Serialized import bytes must be released once the background decoder has consumed them.";

    for (size_t attempt = 0u;
         attempt < 128u && !meshManager.IsResourceRegistered(missingCommittedPath.string());
         ++attempt)
    {
        meshManager.PumpAsyncLoadsForExactPaths({missingCommittedPath.string()}, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(meshManager.IsResourceRegistered(missingCommittedPath.string()))
        << "The prepared request must not require a committed file read.";
    EXPECT_FALSE(meshManager.IsAsyncArtifactLoadFailedExactPath(missingCommittedPath.string()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, PreviewArtifactRequestSurvivesFullLegacyQueueForAllManagers)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async resource request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    EnsureThumbnailPerformanceTestDriver();
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");

    constexpr size_t kLegacyQueueCapacity = 256u;
    const auto meshRoot = root / "Assets" / "QueueMeshes";
    const auto materialRoot = root / "Assets" / "QueueMaterials";
    const auto textureRoot = root / "Assets" / "QueueTextures";
    for (size_t index = 0u; index < kLegacyQueueCapacity + 1u; ++index)
    {
        WriteBinaryFile(
            meshRoot / ("mesh-" + std::to_string(index) + ".nmesh"),
            NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
        WriteNativeArtifactTextFile(
            materialRoot / ("material-" + std::to_string(index) + ".nmat"),
            NLS::Core::Assets::ArtifactType::Material,
            "material",
            1u,
            "shaderLabMaterialVersion=1\n"
            "shader=?\n"
            "surfaceMode=Opaque\n");
        WriteBinaryFile(
            textureRoot / ("texture-" + std::to_string(index) + ".ntex"),
            NLS::Render::Assets::SerializeTextureArtifact(OnePixelTextureArtifact()));
    }

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    TextureManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    MaterialManager materialManager;
    TextureManager textureManager;

    for (size_t index = 0u; index < kLegacyQueueCapacity; ++index)
    {
        EXPECT_EQ(
            meshManager.RequestAsyncArtifact(
                (meshRoot / ("mesh-" + std::to_string(index) + ".nmesh")).string()),
            nullptr);
        EXPECT_EQ(
            materialManager.RequestAsyncArtifact(
                (materialRoot / ("material-" + std::to_string(index) + ".nmat")).string()),
            nullptr);
        EXPECT_EQ(
            textureManager.RequestAsyncArtifact(
                (textureRoot / ("texture-" + std::to_string(index) + ".ntex")).string()),
            nullptr);
    }

    const auto previewMeshPath = meshRoot / "mesh-256.nmesh";
    const auto previewMaterialPath = materialRoot / "material-256.nmat";
    const auto previewTexturePath = textureRoot / "texture-256.ntex";
    EXPECT_EQ(meshManager.RequestAsyncArtifactForPreview(previewMeshPath.string()), nullptr);
    EXPECT_EQ(materialManager.RequestAsyncArtifactForPreview(previewMaterialPath.string()), nullptr);
    EXPECT_EQ(textureManager.RequestAsyncArtifactForPreview(previewTexturePath.string()), nullptr);

    EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPending(previewMeshPath.string()));
    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadPending(previewMaterialPath.string()));
    EXPECT_TRUE(textureManager.IsAsyncArtifactLoadPending(previewTexturePath.string()));
    EXPECT_GE(MeshManager::GetAsyncArtifactRequestDiagnostics().previewRequests, 1u);
    EXPECT_GE(MaterialManager::GetAsyncArtifactRequestDiagnostics().previewRequests, 1u);
    EXPECT_GE(TextureManager::GetAsyncArtifactRequestDiagnostics().previewRequests, 1u);

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    TextureManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    materialManager.UnloadResources();
    textureManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, OrphanedThumbnailTexturePathDoesNotRemainPending)
{
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(ShouldRetainThumbnailPreviewTexturePathForTesting(
        true, false, false, false, false, false));
    EXPECT_TRUE(ShouldRetainThumbnailPreviewTexturePathForTesting(
        false, false, true, false, false, false));
    EXPECT_TRUE(ShouldRetainThumbnailPreviewTexturePathForTesting(
        false, false, false, true, false, false));
    EXPECT_TRUE(ShouldRetainThumbnailPreviewTexturePathForTesting(
        false, false, false, false, true, false));
    EXPECT_TRUE(ShouldRetainThumbnailPreviewTexturePathForTesting(
        false, false, false, false, false, true));
    EXPECT_FALSE(ShouldRetainThumbnailPreviewTexturePathForTesting(
        false, false, false, false, false, false));
}

TEST(AssetThumbnailBehaviorTests, MeshArtifactPumpQueuesRuntimeUploadWithoutCreatingMeshInline)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect runtime mesh upload state.";
#else
    using namespace NLS::Core::ResourceManagement;

    auto& driver = EnsureThumbnailPerformanceGpuTestDriver();
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto meshPath = root / "Assets" / "runtime-upload-pending.nmesh";
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string()), nullptr);
    ASSERT_TRUE(MeshManager::WaitForAsyncArtifactWorkersForTesting());

    meshManager.PumpAsyncLoadsForPaths({ meshPath.string() }, 1u);

    EXPECT_FALSE(meshManager.IsResourceRegistered(meshPath.string()));
    EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MeshManagerRetiresRuntimeUploadWithEmptyPathWindow)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect runtime mesh upload state.";
#else
    using namespace NLS::Core::ResourceManagement;

    auto& driver = EnsureThumbnailPerformanceGpuTestDriver();
    if (!NLS::Render::Context::DriverRendererAccess::HasExplicitRHI(driver))
        GTEST_SKIP() << "An explicit RHI is required to exercise deferred runtime mesh upload retirement.";

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto meshPath = root / "Assets" / "runtime-upload-empty-window.nmesh";
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string()), nullptr);
    ASSERT_TRUE(MeshManager::WaitForAsyncArtifactWorkersForTesting());

    AsyncArtifactRequestDiagnostics queuedUploadDiagnostics;
    for (size_t attempt = 0u; attempt < 128u; ++attempt)
    {
        meshManager.PumpAsyncLoadsForPaths({ meshPath.string() }, 1u);
        queuedUploadDiagnostics = meshManager.GetAsyncArtifactRequestDiagnosticsForOwner();
        if (queuedUploadDiagnostics.runtimeUploadPendingRequests == 1u)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(queuedUploadDiagnostics.runtimeUploadPendingRequests, 1u);

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    EXPECT_EQ(NLS::Render::Context::Detail::RecordPendingMeshRuntimeUploads(*impl, false), 1u);

    const auto recordedUploadDiagnostics =
        NLS::Render::Context::DriverResourceAccess::GetMeshRuntimeUploadDiagnostics(driver);
    ASSERT_EQ(recordedUploadDiagnostics.completedResultCount, 1u);

    // The normal preview inspection can have no paths left once the RHI has
    // recorded the upload. The runtime-upload retirement pass must still run.
    meshManager.PumpAsyncLoadsForPaths({}, 1u);

    EXPECT_TRUE(meshManager.IsResourceRegistered(meshPath.string()));
    EXPECT_FALSE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()));
    EXPECT_EQ(
        NLS::Render::Context::DriverResourceAccess::GetMeshRuntimeUploadDiagnostics(driver).completedResultCount,
        0u);

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, ExactPendingMeshLookupDoesNotResolveArtifactPathAgain)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect mesh path resolution.";
#else
    using namespace NLS::Core::ResourceManagement;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto meshPath = root / "Assets" / "pending-lookup.nmesh";
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string()), nullptr);
    MeshManager::ResetArtifactResourcePathResolutionCountForTesting();

    for (size_t attempt = 0u; attempt < 64u; ++attempt)
    {
        EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()));
        EXPECT_FALSE(meshManager.IsAsyncArtifactLoadFailed(meshPath.string()));
    }

    EXPECT_EQ(MeshManager::GetArtifactResourcePathResolutionCountForTesting(), 0u)
        << "Exact pending and non-failed lookups run on the thumbnail UI pump and must stay in-memory.";

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MeshArtifactTypeProbeIgnoresLargeMetadataPayload)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto meshPath = root / "large-metadata.nmesh";
    const auto serialized = NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact());
    const auto container = ReadNativeArtifactContainer(serialized, ArtifactType::Mesh, 3u);
    ASSERT_TRUE(container.has_value());

    auto metadata = container->metadata;
    metadata.displayName.assign(70u * 1024u, 'M');
    WriteBinaryFile(meshPath, WriteNativeArtifactContainer(std::move(metadata), container->payload));

    EXPECT_FALSE(NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
        meshPath,
        64u * 1024u).has_value());
    EXPECT_TRUE(NLS::Render::Assets::IsMeshArtifactFile(meshPath))
        << "Artifact type probing must read the fixed container header without parsing metadata.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, AbsoluteMeshArtifactRequestSkipsRegisteredAliasScan)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect mesh path resolution.";
#else
    using namespace NLS::Core::ResourceManagement;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto meshPath = root / "Assets" / "absolute-target.nmesh";
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    for (size_t index = 0u; index < 64u; ++index)
    {
        meshManager.RegisterResource(
            (root / "Assets" / ("registered-" + std::to_string(index) + ".nmesh")).string(),
            new NLS::Render::Resources::Mesh({}, {}, 0u));
    }

    MeshManager::ResetArtifactResourcePathResolutionCountForTesting();
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string()), nullptr);
    EXPECT_EQ(MeshManager::GetArtifactResourcePathResolutionCountForTesting(), 1u)
        << "An absolute artifact path is already canonical and must not scan every registered mesh alias.";

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MeshManagerKeepsAsyncArtifactQueuedUntilJobSystemExecutorIsAvailable)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async mesh request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    ResetThumbnailPerformanceJobSystem();
    EnsureThumbnailPerformanceTestDriver();
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto meshPath = root / "Assets" / "queued.nmesh";
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string()), nullptr);
    EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()));

    meshManager.PumpAsyncLoadsForPaths({meshPath.string()}, 8u);
    EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()))
        << "Async artifact requests must not synchronously load or fail when the JobSystem executor is unavailable.";
    EXPECT_FALSE(meshManager.IsResourceRegistered(meshPath.string()));

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    for (size_t attempt = 0; attempt < 128u && !meshManager.IsResourceRegistered(meshPath.string()); ++attempt)
    {
        meshManager.PumpAsyncLoadsForPaths({meshPath.string()}, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(meshManager.IsResourceRegistered(meshPath.string()));
    EXPECT_FALSE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MeshManagerReadyUnrelatedAsyncArtifactsDoNotBlockPathFilteredPromotion)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async mesh request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    EnsureThumbnailPerformanceTestDriver();
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto newPath = root / "Assets" / "new-target.nmesh";
    WriteBinaryFile(newPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    for (size_t index = 0u; index < 8u; ++index)
    {
        const auto stalePath = root / "Assets" / ("stale" + std::to_string(index) + ".nmesh");
        WriteBinaryFile(stalePath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
        EXPECT_EQ(meshManager.RequestAsyncArtifact(stalePath.string()), nullptr);
    }
    EXPECT_EQ(MeshManager::GetTotalAsyncArtifactRequestCountForTesting(), 8u);

    for (size_t attempt = 0u; attempt < 128u; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    EXPECT_EQ(meshManager.RequestAsyncArtifact(newPath.string()), nullptr);
    for (size_t attempt = 0u; attempt < 128u && !meshManager.IsResourceRegistered(newPath.string()); ++attempt)
    {
        meshManager.PumpAsyncLoadsForPaths({newPath.string()}, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(meshManager.IsResourceRegistered(newPath.string()))
        << "Completed stale requests must not keep the active cap full for a new path-filtered request.";

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MeshManagerPromotesQueuedPathBeforeBudgetStop)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async mesh request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    ResetThumbnailPerformanceJobSystem();
    EnsureThumbnailPerformanceTestDriver();
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");

    const auto meshPath = root / "Assets" / "queued-preview.nmesh";
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    EXPECT_EQ(meshManager.RequestAsyncArtifactForPreview(meshPath.string()), nullptr);

    const auto queuedBefore = MeshManager::GetAsyncArtifactRequestDiagnostics();
    ASSERT_EQ(queuedBefore.previewQueuedRequests, 1u)
        << "A request submitted before the executor exists must remain queued.";

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());

    meshManager.PumpAsyncLoadsForPaths(
        { meshPath.string() },
        8u,
        []
        {
            return true;
        });

    const auto queuedAfter = MeshManager::GetAsyncArtifactRequestDiagnostics();
    EXPECT_EQ(queuedAfter.previewQueuedRequests, 0u)
        << "A budget stop must not prevent queued preview work from being started.";

    for (size_t attempt = 0u;
         attempt < 128u && !meshManager.IsResourceRegistered(meshPath.string());
         ++attempt)
    {
        meshManager.PumpAsyncLoadsForPaths({ meshPath.string() }, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(meshManager.IsResourceRegistered(meshPath.string()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MeshManagerSharedRequestRevivesCanceledInFlightArtifactBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async mesh request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    EnsureThumbnailPerformanceTestDriver();
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto meshPath = root / "Assets" / "revive-canceled.nmesh";
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string(), true), nullptr);
    meshManager.CancelAsyncArtifact(meshPath.string());

    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string(), false), nullptr);
    for (size_t attempt = 0u; attempt < 128u && !meshManager.IsResourceRegistered(meshPath.string()); ++attempt)
    {
        meshManager.PumpAsyncLoadsForPaths({meshPath.string()}, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(meshManager.IsResourceRegistered(meshPath.string()))
        << "A commit/shared request must not inherit a hover cancellation that arrived before pump consumed the worker result.";
    EXPECT_FALSE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MeshManagerCancelableRequestRevivesCanceledInFlightArtifactBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async mesh request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    EnsureThumbnailPerformanceTestDriver();
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto meshPath = root / "Assets" / "revive-cancelable.nmesh";
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string(), true), nullptr);
    meshManager.CancelAsyncArtifact(meshPath.string());
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string(), true), nullptr);
    for (size_t attempt = 0u; attempt < 128u && !meshManager.IsResourceRegistered(meshPath.string()); ++attempt)
    {
        meshManager.PumpAsyncLoadsForPaths({meshPath.string()}, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(meshManager.IsResourceRegistered(meshPath.string()))
        << "A renewed hover request must not inherit the previous hover cancellation.";
    EXPECT_FALSE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()));
    EXPECT_FALSE(meshManager.IsAsyncArtifactLoadFailed(meshPath.string()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MaterialManagerPumpAsyncLoadsForPathsLeavesUnrelatedThumbnailRequestsPending)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const auto targetArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("e001000000000000000000000000000000000000000000000000000000000001");
    const auto unrelatedArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("e002000000000000000000000000000000000000000000000000000000000002");
    const auto targetPath = root / targetArtifactPath;
    const auto unrelatedPath = root / unrelatedArtifactPath;
    WriteNativeArtifactTextFile(
        targetPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");
    WriteNativeArtifactTextFile(
        unrelatedPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager materialManager;
    EXPECT_EQ(materialManager.RequestAsyncArtifact(targetPath.string()), nullptr);
    EXPECT_EQ(materialManager.RequestAsyncArtifact(unrelatedPath.string()), nullptr);

    for (size_t attempt = 0; attempt < 64u && MaterialManager::GetPendingAsyncArtifactRequestCountForTesting() > 1u; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    materialManager.PumpAsyncLoadsForPaths({targetPath.string()}, 8u);

    EXPECT_FALSE(materialManager.IsAsyncArtifactLoadPending(targetPath.string()));
    EXPECT_FALSE(materialManager.IsResourceRegistered(unrelatedPath.string()));
    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadPending(unrelatedPath.string()));

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    materialManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, ResourceManagerThumbnailPumpsHonorStopBeforeCompletion)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async resource request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    EnsureThumbnailPerformanceTestDriver();
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");

    const auto meshPath = root / "Assets" / "deadline-mesh.nmesh";
    const auto materialPath = root / "Assets" / "deadline-material.nmat";
    const auto texturePath = root / "Assets" / "deadline-texture.ntex";
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");
    WriteBinaryFile(texturePath, NLS::Render::Assets::SerializeTextureArtifact(OnePixelTextureArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    TextureManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    MaterialManager materialManager;
    TextureManager textureManager;
    ASSERT_EQ(meshManager.RequestAsyncArtifact(meshPath.string()), nullptr);
    ASSERT_EQ(materialManager.RequestAsyncArtifact(materialPath.string()), nullptr);
    ASSERT_EQ(textureManager.RequestAsyncArtifact(texturePath.string()), nullptr);

    const auto stopImmediately = []()
    {
        return true;
    };
    meshManager.PumpAsyncLoadsForPaths({ meshPath.string() }, 8u, stopImmediately);
    materialManager.PumpAsyncLoadsForPaths({ materialPath.string() }, 8u, stopImmediately);
    textureManager.PumpAsyncLoadsForPaths({ texturePath.string() }, 8u, stopImmediately);

    EXPECT_FALSE(meshManager.IsResourceRegistered(meshPath.string()));
    EXPECT_FALSE(materialManager.IsResourceRegistered(materialPath.string()));
    EXPECT_FALSE(textureManager.IsResourceRegistered(texturePath.string()));
    EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()));
    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadPending(materialPath.string()));
    EXPECT_TRUE(textureManager.IsAsyncArtifactLoadPending(texturePath.string()));

    // Thumbnail continuation may spend its bounded budget on inspection before
    // the worker future becomes ready. Once the future is ready, the opt-in
    // completion path must be able to retire it even when the deadline
    // predicate is already tripped, without turning the pump into a wait.
    for (size_t attempt = 0u; attempt < 256u; ++attempt)
    {
        const auto meshDiagnostics = MeshManager::GetAsyncArtifactRequestDiagnostics();
        const auto materialDiagnostics = MaterialManager::GetAsyncArtifactRequestDiagnostics();
        const auto textureDiagnostics = TextureManager::GetAsyncArtifactRequestDiagnostics();
        if (meshDiagnostics.readyRequests > 0u &&
            materialDiagnostics.readyRequests > 0u &&
            textureDiagnostics.readyRequests > 0u)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_GT(MeshManager::GetAsyncArtifactRequestDiagnostics().readyRequests, 0u);
    EXPECT_GT(MaterialManager::GetAsyncArtifactRequestDiagnostics().readyRequests, 0u);
    EXPECT_GT(TextureManager::GetAsyncArtifactRequestDiagnostics().readyRequests, 0u);

    meshManager.PumpAsyncLoadsForPaths({ meshPath.string() }, 8u, stopImmediately, true);
    materialManager.PumpAsyncLoadsForPaths({ materialPath.string() }, 8u, stopImmediately, true);
    textureManager.PumpAsyncLoadsForPaths({ texturePath.string() }, 8u, stopImmediately, true);

    EXPECT_TRUE(meshManager.IsResourceRegistered(meshPath.string()));
    EXPECT_FALSE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()));
    EXPECT_FALSE(materialManager.IsAsyncArtifactLoadPending(materialPath.string()));
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(texturePath.string()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    TextureManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    materialManager.UnloadResources();
    textureManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MaterialManagerSharedRequestRevivesCanceledInFlightArtifactBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const auto materialArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("e003000000000000000000000000000000000000000000000000000000000003");
    const auto materialPath = root / materialArtifactPath;
    WriteThumbnailPerformanceAsyncMaterialShader(root);
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/AsyncMaterial.shader\n"
        "surfaceMode=Opaque\n");

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    ShaderManager shaderManager;
    TextureManager textureManager;
    MaterialManager materialManager;
    ScopedServiceOverride<ShaderManager> shaderManagerOverride(shaderManager);
    ScopedServiceOverride<TextureManager> textureManagerOverride(textureManager);
    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string(), true), nullptr);
    materialManager.CancelAsyncArtifact(materialPath.string());

    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string(), false), nullptr);
    for (size_t attempt = 0u; attempt < 128u && !materialManager.IsResourceRegistered(materialPath.string()); ++attempt)
    {
        materialManager.PumpAsyncLoadsForPaths({materialPath.string()}, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(materialManager.IsResourceRegistered(materialPath.string()))
        << "A commit/shared request must not inherit a hover cancellation that arrived before pump consumed the worker result.";
    EXPECT_FALSE(materialManager.IsAsyncArtifactLoadPending(materialPath.string()));

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    materialManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MaterialManagerCancelableRequestRevivesCanceledInFlightArtifactBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const auto materialArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("e004000000000000000000000000000000000000000000000000000000000004");
    const auto materialPath = root / materialArtifactPath;
    WriteThumbnailPerformanceAsyncMaterialShader(root);
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/AsyncMaterial.shader\n"
        "surfaceMode=Opaque\n");

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    ShaderManager shaderManager;
    TextureManager textureManager;
    MaterialManager materialManager;
    ScopedServiceOverride<ShaderManager> shaderManagerOverride(shaderManager);
    ScopedServiceOverride<TextureManager> textureManagerOverride(textureManager);
    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string(), true), nullptr);
    materialManager.CancelAsyncArtifact(materialPath.string());
    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string(), true), nullptr);
    for (size_t attempt = 0u; attempt < 128u && !materialManager.IsResourceRegistered(materialPath.string()); ++attempt)
    {
        materialManager.PumpAsyncLoadsForPaths({materialPath.string()}, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(materialManager.IsResourceRegistered(materialPath.string()))
        << "A renewed hover request must not inherit the previous hover cancellation.";
    EXPECT_FALSE(materialManager.IsAsyncArtifactLoadPending(materialPath.string()));
    EXPECT_FALSE(materialManager.IsAsyncArtifactLoadFailed(materialPath.string()));

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    materialManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, MaterialManagerAsyncArtifactRejectsPlaceholderShaderReference)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const auto materialArtifactPath =
        ThumbnailPerformanceLibraryArtifactPath("e005000000000000000000000000000000000000000000000000000000000005");
    const auto materialPath = root / materialArtifactPath;
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n");

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager materialManager;
    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string(), false), nullptr);
    for (size_t attempt = 0u;
         attempt < 128u &&
            !materialManager.IsAsyncArtifactLoadFailed(materialPath.string());
         ++attempt)
    {
        materialManager.PumpAsyncLoadsForPaths({materialPath.string()}, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_FALSE(materialManager.IsResourceRegistered(materialPath.string()));
    EXPECT_FALSE(materialManager.IsAsyncArtifactLoadPending(materialPath.string()));
    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadFailed(materialPath.string()));

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    materialManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, TextureManagerSharedRequestRevivesCanceledInFlightArtifactBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async texture request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    const auto gpu = EnsureDeterministicThumbnailGpuTestDriver();
    const size_t textureCreateCallsBeforeRequest = gpu.device->textureCreateCalls;
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        (root / "Assets").string() + "/",
        (root / "EngineAssets").string() + "/");
    const auto texturePath = root / "Assets" / "revive-canceled.ntex";
    WriteBinaryFile(texturePath, NLS::Render::Assets::SerializeTextureArtifact(OnePixelTextureArtifact()));

    TextureManager::ClearAsyncArtifactRequestStateForTesting();
    TextureManager textureManager;
    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    textureManager.CancelAsyncArtifact(texturePath.string());

    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), false), nullptr);
    for (size_t attempt = 0u; attempt < 128u && !textureManager.IsResourceRegistered(texturePath.string()); ++attempt)
    {
        textureManager.PumpAsyncLoadsForPaths({texturePath.string()}, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(textureManager.IsResourceRegistered(texturePath.string()))
        << "A commit/shared request must not inherit a hover cancellation that arrived before pump consumed the worker result.";
    EXPECT_GT(gpu.device->textureCreateCalls, textureCreateCallsBeforeRequest)
        << "Revival must complete the runtime texture upload through the deterministic explicit device.";
    EXPECT_TRUE(gpu.device->lastUploadHadData);
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(texturePath.string()));

    TextureManager::ClearAsyncArtifactRequestStateForTesting();
    textureManager.UnloadResources();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, TextureManagerCancelableRequestRevivesCanceledInFlightArtifactBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async texture request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    const auto gpu = EnsureDeterministicThumbnailGpuTestDriver();
    const size_t textureCreateCallsBeforeRequest = gpu.device->textureCreateCalls;
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        (root / "Assets").string() + "/",
        (root / "EngineAssets").string() + "/");
    const auto texturePath = root / "Assets" / "revive-cancelable.ntex";
    WriteBinaryFile(texturePath, NLS::Render::Assets::SerializeTextureArtifact(OnePixelTextureArtifact()));

    TextureManager::ClearAsyncArtifactRequestStateForTesting();
    TextureManager textureManager;
    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    textureManager.CancelAsyncArtifact(texturePath.string());
    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    for (size_t attempt = 0u; attempt < 128u && !textureManager.IsResourceRegistered(texturePath.string()); ++attempt)
    {
        textureManager.PumpAsyncLoadsForPaths({texturePath.string()}, 8u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(textureManager.IsResourceRegistered(texturePath.string()))
        << "A renewed hover request must not inherit the previous hover cancellation.";
    EXPECT_GT(gpu.device->textureCreateCalls, textureCreateCallsBeforeRequest)
        << "Revival must complete the runtime texture upload through the deterministic explicit device.";
    EXPECT_TRUE(gpu.device->lastUploadHadData);
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(texturePath.string()));
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadFailed(texturePath.string()));

    TextureManager::ClearAsyncArtifactRequestStateForTesting();
    textureManager.UnloadResources();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, GpuPreviewReadbackPendingIsRepolledByRendererPump)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    NLS::Core::Assets::ClearArtifactLoadTelemetry();

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    PendingThenReadyPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(pending->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForGpu);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto repolled = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(repolled.has_value());
    EXPECT_EQ(repolled->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(repolled->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(renderer.renderCount, 2u);
    EXPECT_TRUE(service.HasInFlightRequest())
        << "The second renderer pump completed the pending readback and queued the cache write.";

    std::optional<AssetThumbnailServiceResult> completed;
    for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
    {
        completed = service.ConsumeCompletedThumbnail();
        if (!completed.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Ready);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, MissingGpuPreviewLaneMembershipIsRepairedBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to simulate a missing lane entry.";
#else
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    StubPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u);

    service.DropGpuPreviewQueueLaneMembershipForTesting(request);
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u)
        << "The request truth table must survive loss of its lane membership.";

    const auto repaired = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(repaired.has_value())
        << "The scheduler must repair a queued request before selecting work.";
    EXPECT_EQ(repaired->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(repaired->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(renderer.renderCount, 1u);

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, GpuPreviewUsesPolledReadbackWithoutRerendering)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    PolledReadbackPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto submitted = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(submitted->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(submitted->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForGpu);
    EXPECT_EQ(renderer.submitCount, 1u);
    EXPECT_EQ(renderer.legacyRenderCount, 0u);

    // Asset Browser rebuilds requests while a thumbnail is pending. The new
    // UI revision must coalesce with the existing GPU/readback work instead of
    // submitting the same visual preview again.
    const auto duplicateRequest = service.GetThumbnail(request);
    EXPECT_EQ(duplicateRequest.status, AssetThumbnailServiceStatus::Pending);

    const auto polled = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(polled.has_value());
    EXPECT_EQ(polled->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(polled->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(renderer.pollCount, 2u)
        << "The service must poll readbacks before selecting the next preview.";
    EXPECT_EQ(renderer.maxPollCount, 3u)
        << "The readback ring poll must use the bounded three-slot batch.";
    EXPECT_EQ(renderer.submitCount, 1u)
        << "A completed readback must be consumed without submitting the preview again.";
    EXPECT_EQ(renderer.legacyRenderCount, 0u);

    std::optional<AssetThumbnailServiceResult> completed;
    for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
    {
        completed = service.ConsumeCompletedThumbnail();
        if (!completed.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Ready);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, PendingReadbackWaitsForFenceWithoutResubmittingPreview)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    PolledReadbackPreviewRenderer renderer;
    renderer.delayFirstPoll = true;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto submitted = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(submitted->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(renderer.submitCount, 1u);

    // The first fence poll is deliberately incomplete. The service should keep
    // the readback slot pending and avoid re-entering the renderer submission
    // path until PollCompletedReadbacks returns pixels.
    EXPECT_FALSE(service.GenerateNextThumbnail(renderer).has_value());
    EXPECT_EQ(renderer.pollCount, 2u);
    EXPECT_EQ(renderer.submitCount, 1u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForGpu);

    const auto completedReadback = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(completedReadback.has_value());
    EXPECT_EQ(completedReadback->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(renderer.submitCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, PendingAsyncReadbackDoesNotBlockOtherVisibleGpuWork)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto firstRequest = MakeGpuPreviewRequest(root);
    auto secondRequest = MakeGpuPreviewRequest(root);
    secondRequest.assetId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic(
        "thumbnail-performance-second-gpu-preview"));
    secondRequest.sourceAssetPath = "Assets/Models/Second.fbx";
    secondRequest.subAssetKey = "mesh:Second";
    PolledReadbackPreviewRenderer renderer;
    renderer.delayFirstPoll = true;
    AssetThumbnailService service;

    ASSERT_EQ(service.GetThumbnail(firstRequest).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(secondRequest).status, AssetThumbnailServiceStatus::Pending);

    const auto firstSubmitted = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(firstSubmitted.has_value());
    EXPECT_EQ(firstSubmitted->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(renderer.submitCount, 1u);

    // The first fence is deliberately incomplete. A second visible request
    // must still use the submit lane instead of being blocked by the first
    // request's readback ownership entry.
    const auto secondSubmitted = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(secondSubmitted.has_value());
    EXPECT_EQ(secondSubmitted->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(renderer.pollCount, 2u);
    EXPECT_EQ(renderer.submitCount, 2u);
    EXPECT_EQ(renderer.legacyRenderCount, 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ThumbnailRendererRegistryForwardsReadbackLifecycle)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    auto renderer = std::make_shared<PolledReadbackPreviewRenderer>();
    ThumbnailRendererRegistry registry;
    registry.Register(request.kind, renderer);
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto submitted = service.GenerateNextThumbnail(registry);
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(submitted->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(renderer->submitCount, 1u);

    const auto completed = service.GenerateNextThumbnail(registry);
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(renderer->pollCount, 2u);
    EXPECT_EQ(renderer->submitCount, 1u);

    std::optional<AssetThumbnailServiceResult> persisted;
    for (int attempt = 0; attempt < 100 && !persisted.has_value(); ++attempt)
    {
        persisted = service.ConsumeCompletedThumbnail();
        if (!persisted.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->status, AssetThumbnailServiceStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, InvalidatedReadbackOrphanCarriesRequestRevision)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    OrphanTrackingPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto submitted = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(submitted->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_NE(renderer.submittedRevision, 0u);
    ASSERT_EQ(renderer.lastTicket.requestRevision, renderer.submittedRevision);

    service.InvalidateThumbnail(request);
    EXPECT_TRUE(service.IsPresentationInvalidated(request));

    EXPECT_FALSE(service.GenerateNextThumbnail(renderer).has_value());
    EXPECT_EQ(renderer.orphanedTicket.requestKey, renderer.lastTicket.requestKey);
    EXPECT_EQ(renderer.orphanedTicket.requestRevision, renderer.submittedRevision);
    EXPECT_NE(renderer.orphanedTicket.requestRevision, 0u)
        << "An invalidated readback must be matched by exact revision, not only by cache key.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, RetryableGpuPreviewFailureIsRequeuedByRendererPump)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    RetryableFailureThenReadyPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto fallback = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(fallback.has_value());
    EXPECT_EQ(fallback->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(
        fallback->diagnostic,
        "thumbnail-gpu-preview-readback-failed:previous async readback has not been completed");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForGpu);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u)
        << "A previous async readback that has not completed yet must stay on the GPU readback polling path.";

    const auto repolled = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(repolled.has_value());
    EXPECT_EQ(repolled->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(repolled->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(renderer.renderCount, 2u);

    std::optional<AssetThumbnailServiceResult> completed;
    for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
    {
        completed = service.ConsumeCompletedThumbnail();
        if (!completed.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Ready);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPreviewInFlightCacheWriteIsCancelledDuringServiceShutdown)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    const auto cacheEntry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(cacheEntry.has_value());

    {
        StubPreviewRenderer renderer;
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
        const auto pending = service.GenerateNextThumbnail(renderer);
        ASSERT_TRUE(pending.has_value());
        ASSERT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
        EXPECT_TRUE(service.HasInFlightRequest());
    }

    const auto evaluation = EvaluateAssetThumbnailCache(request);
    EXPECT_NE(evaluation.status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, PreviewLoadPolicyAvoidsRuntimeLifecycleAndSynchronousResourceWaits)
{
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Serialize;

    const auto policy = BuildEditorThumbnailPreviewLoadPolicy();

    EXPECT_TRUE(policy.deferAssetReferenceResolution);
    EXPECT_TRUE(policy.suppressGameObjectCreatedEvents);
    EXPECT_TRUE(policy.deferActivation);
    EXPECT_FALSE(policy.synchronousAssetReferencePrewarm);
    EXPECT_FALSE(policy.rebuildRuntimeCachesAfterLoad);
    EXPECT_EQ(policy.missingAssetPolicy, MissingAssetPolicy::Preserve);
    EXPECT_EQ(policy.invalidReferencePolicy, InvalidReferencePolicy::Fail);
    EXPECT_EQ(policy.unknownTypePolicy, UnknownTypePolicy::Fail);
}

TEST(AssetThumbnailBehaviorTests, PreviewRenderableSnapshotUsesPrefabGraphDependenciesWithoutInstantiatingPrefab)
{
    using namespace NLS::Editor::Assets;

    const auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto snapshot = BuildPreviewRenderableSnapshot(prefab);

    ASSERT_EQ(snapshot.drawItems.size(), 1u);
    EXPECT_EQ(
        snapshot.drawItems.front().meshPath,
        "Library/Artifacts/50505050-5050-4050-8050-505050505050/Hero.nmesh");
    ASSERT_EQ(snapshot.drawItems.front().materialPaths.size(), 1u);
    EXPECT_EQ(snapshot.drawItems.front().materialPaths.front(), "Assets/Materials/Hero.mat");
    EXPECT_EQ(snapshot.drawItems.front().localPosition.x, 3.0f);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.y, 4.0f);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.z, 5.0f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.x, 2.0f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.y, 2.5f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.z, 3.0f);
}

TEST(AssetThumbnailBehaviorTests, PrefabPreviewResourcePlanDeduplicatesRepeatedDrawDependencies)
{
    using namespace NLS::Editor::Assets;

    constexpr size_t kSponzaLikeDrawItems = 405u;

    PreviewRenderableSnapshot snapshot;
    snapshot.drawItems.reserve(kSponzaLikeDrawItems);
    for (size_t index = 0u; index < kSponzaLikeDrawItems; ++index)
    {
        PreviewDrawItem item;
        item.meshPath = "Library/Artifacts/aa/shared-arch.nmesh";
        item.materialPaths = {
            "Library/Artifacts/bb/shared-stone.nmat",
            "Library/Artifacts/cc/shared-trim.nmat"
        };
        item.localPosition = {static_cast<float>(index), 0.0f, 0.0f};
        snapshot.drawItems.push_back(std::move(item));
    }
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();

    AssetThumbnailRequest request;
    request.kind = AssetThumbnailKind::PrefabPreview;

    const auto plan = BuildThumbnailPreviewPrefabResourcePlanForTesting(request, snapshot);

    EXPECT_EQ(plan.drawItemCount, kSponzaLikeDrawItems)
        << "UE-style thumbnail scheduling must preserve the complete prefab instead of dropping draw items.";
    EXPECT_EQ(plan.uniqueMeshLoadPathCount, 1u)
        << "Large imported prefab thumbnails must not re-resolve the same mesh once per draw item.";
    EXPECT_EQ(plan.uniqueMaterialLoadPathCount, 2u)
        << "Repeated material slots should be resolved once and then reused across prefab draw items.";
    EXPECT_EQ(plan.dependencyDrawItemInspectionCount, kSponzaLikeDrawItems);
    ASSERT_TRUE(plan.hasFullWorldBounds);
    EXPECT_GT(plan.fullWorldBoundsMax.x, 400.0f)
        << "Proxy selection must retain transform-only full-scene bounds for spatial sampling.";
}

TEST(AssetThumbnailBehaviorTests, PrefabPreviewSceneAssemblyPendingPreservesSceneObjects)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect preview assembly policy.";
#else
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
        "thumbnail-gpu-preview-resources-pending:prefab-scene-assembly=240/405"));
    EXPECT_TRUE(ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
        "thumbnail-gpu-preview-readback-pending"));
    EXPECT_TRUE(ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
        "thumbnail-gpu-preview-resources-pending:prefab-draw-prewarm=12/405"));
    EXPECT_TRUE(ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
        "thumbnail-gpu-preview-resident-partial"));
    EXPECT_FALSE(ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
        "thumbnail-gpu-preview-resources-pending:resolved-mesh-cache=1"));
#endif
}

TEST(AssetThumbnailBehaviorTests, ExactPathMeshPumpSkipsRepeatedArtifactPathResolution)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect mesh path resolution.";
#else
    using namespace NLS::Core::ResourceManagement;

    EnsureThumbnailPerformanceTestDriver();
    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    const auto meshPath = root / "Assets" / "exact-path-pump.nmesh";
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string()), nullptr);
    ASSERT_TRUE(MeshManager::WaitForAsyncArtifactWorkersForTesting());
    MeshManager::ResetArtifactResourcePathResolutionCountForTesting();

    EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPendingExactPath(meshPath.string()));
    EXPECT_FALSE(meshManager.IsAsyncArtifactLoadFailedExactPath(meshPath.string()));
    EXPECT_FALSE(meshManager.IsAsyncArtifactLoadPendingExactPath((root / "not-started.nmesh").string()));
    EXPECT_FALSE(meshManager.IsAsyncArtifactLoadFailedExactPath((root / "not-started.nmesh").string()));
    meshManager.PumpAsyncLoadsForExactPaths({meshPath.string()}, 1u);

    EXPECT_EQ(MeshManager::GetArtifactResourcePathResolutionCountForTesting(), 0u)
        << "Thumbnail resource plans already hold exact request keys and must not resolve artifact paths each frame.";

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, SmallColdPrefabRemainsEligibleForImmediatePreview)
{
    using namespace NLS::Editor::Assets;

    EXPECT_FALSE(ShouldDeferLargePrefabPreviewUntilResidentForTesting(64u, false));
    EXPECT_TRUE(ShouldDeferLargePrefabPreviewUntilResidentForTesting(65u, false));
}

TEST(AssetThumbnailBehaviorTests, ResidentLargePrefabAlwaysUsesLoadedSceneResources)
{
    using namespace NLS::Editor::Assets;

    EXPECT_FALSE(ShouldDeferLargePrefabPreviewUntilResidentForTesting(2048u, true));
}

TEST(AssetThumbnailBehaviorTests, PrefabPreviewProxyUsesRealMeshBoundsForCollapsedNodeTransforms)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    constexpr size_t kDrawItemCount = 96u;
    constexpr size_t kDominantDrawItemIndex = 48u;

    PreviewRenderableSnapshot snapshot;
    snapshot.drawItems.reserve(kDrawItemCount);
    for (size_t index = 0u; index < kDrawItemCount; ++index)
    {
        const auto meshPath = root / "Assets" / ("collapsed-mesh-" + std::to_string(index) + ".nmesh");
        auto mesh = TriangleMeshArtifact();
        mesh.boundingSphere.position = index == kDominantDrawItemIndex
            ? NLS::Maths::Vector3(50.0f, 0.0f, 0.0f)
            : NLS::Maths::Vector3(0.0f, 0.0f, 0.0f);
        mesh.boundingSphere.radius = index == kDominantDrawItemIndex ? 25.0f : 1.0f;
        WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(mesh));

        PreviewDrawItem item;
        item.meshPath = meshPath.generic_string();
        snapshot.drawItems.push_back(std::move(item));
    }
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.kind = AssetThumbnailKind::PrefabPreview;
    const auto plan = BuildThumbnailPreviewPrefabResourcePlanForTesting(request, snapshot);

    EXPECT_EQ(plan.drawItemCount, kDrawItemCount);
    EXPECT_NE(
        std::find(
            plan.selectedDrawItemIndices.begin(),
            plan.selectedDrawItemIndices.end(),
            kDominantDrawItemIndex),
        plan.selectedDrawItemIndices.end())
        << "The final proxy must retain a dominant mesh discovered from artifact-header bounds.";
    EXPECT_EQ(plan.dependencyDrawItemInspectionCount, kDrawItemCount);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, ThumbnailPreviewProxyLeaseIsSafeAfterPoolDestruction)
{
    using namespace NLS::Editor::Assets;

    std::optional<ThumbnailPreviewProxyPool::Lease> lateLease;
    {
        NLS::Engine::SceneSystem::Scene scene;
        {
            ThumbnailPreviewProxyPool pool(scene, 1u);
            auto acquired = pool.Acquire("thumbnail-lifetime-test");
            ASSERT_TRUE(acquired.has_value());
            ASSERT_NE(acquired->Get(), nullptr);
            lateLease = std::move(*acquired);
        }

        ASSERT_TRUE(lateLease.has_value());
        EXPECT_EQ(lateLease->Get(), nullptr)
            << "A lease retained by a readback must become inert when its pool is destroyed.";
        EXPECT_FALSE(static_cast<bool>(*lateLease));
    }

    ASSERT_TRUE(lateLease.has_value());
    EXPECT_EQ(lateLease->Get(), nullptr)
        << "Late lease release must not dereference the destroyed preview scene.";
    lateLease.reset();
}

TEST(AssetThumbnailBehaviorTests, PrefabPreviewResourcePlanKeepsHighDensityMeshForAsyncGpuLoading)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const std::filesystem::path meshArtifactPath =
        "Library/Artifacts/aa/high-density.nmesh";
    NLS::Render::Assets::MeshArtifactData highDensityMesh;
    highDensityMesh.vertices.resize(250000u);
    highDensityMesh.indices = {0u, 1u, 2u};
    WriteBinaryFile(root / meshArtifactPath, NLS::Render::Assets::SerializeMeshArtifact(highDensityMesh));

    PreviewRenderableSnapshot snapshot;
    PreviewDrawItem item;
    item.meshPath = meshArtifactPath.generic_string();
    snapshot.drawItems.push_back(std::move(item));
    snapshot.expectedDrawItemCount = 1u;

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"));
    request.kind = AssetThumbnailKind::PrefabPreview;

    const auto plan = BuildThumbnailPreviewPrefabResourcePlanForTesting(request, snapshot);

    EXPECT_EQ(plan.drawItemCount, 1u)
        << "A valid high-density mesh must stay in the complete GPU prefab draw set; async upload budgets prevent UI stalls.";
    EXPECT_EQ(plan.uniqueMeshLoadPathCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, AssetPanelThumbnailLoadsSelectedFormalLODFromBundle)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect thumbnail LOD selection.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto path = root / "Assets" / "formal-lod-thumbnail.nmesh";
    auto lod0 = TriangleMeshArtifact();
    lod0.materialIndex = 4u;
    auto lod1 = TriangleMeshArtifact();
    lod1.materialIndex = 7u;
    NLS::Render::Assets::MeshArtifactBundle bundle;
    bundle.lodResources = {
        {std::move(lod0), 2.0f},
        {std::move(lod1), 0.5f}};
    WriteBinaryFile(path, NLS::Render::Assets::SerializeMeshArtifactBundle(bundle));

    const auto selected = LoadThumbnailFormalLODForTesting(path);

    EXPECT_TRUE(selected.loaded);
    EXPECT_EQ(selected.materialIndex, 7u)
        << "The asset panel must use the formal screen-size LOD instead of simplifying LOD0.";
    EXPECT_EQ(selected.indexCount, 3u);
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, AssetPanelThumbnailDoesNotCreatePreviewSampleForLargeLegacyMesh)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect thumbnail LOD selection.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto path = root / "Assets" / "large-legacy-thumbnail.nmesh";
    auto mesh = TriangleMeshArtifact();
    mesh.vertices.resize(250001u);
    WriteBinaryFile(path, NLS::Render::Assets::SerializeMeshArtifact(mesh));

    const auto selected = LoadThumbnailFormalLODForTesting(path);

    EXPECT_FALSE(selected.loaded)
        << "A legacy mesh over the preview budget must fall back instead of generating a temporary simplified model.";
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, PrefabPreviewResourcePlanStopsAtUnreadyDependencyBudget)
{
    using namespace NLS::Editor::Assets;

    PreviewRenderableSnapshot snapshot;
    for (size_t index = 0u; index < 32u; ++index)
    {
        PreviewDrawItem item;
        item.meshPath = "Library/Artifacts/mesh-" + std::to_string(index) + "/chunk.nmesh";
        item.materialPaths = {
            "Library/Artifacts/material-" + std::to_string(index) + "/surface.nmat"
        };
        snapshot.drawItems.push_back(std::move(item));
    }
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();

    AssetThumbnailRequest request;
    request.kind = AssetThumbnailKind::PrefabPreview;

    const auto plan = BuildThumbnailPreviewPrefabResourcePlanForTesting(
        request,
        snapshot,
        4u);

    EXPECT_TRUE(plan.truncatedForPendingResources)
        << "Pending large prefab previews must slice resource planning before walking every draw dependency.";
    EXPECT_LE(plan.uniqueMeshLoadPathCount, 4u)
        << "Only mesh resources are hard prefab GPU preview dependencies; materials may be queued opportunistically and fall back to the default preview material.";
    EXPECT_LT(plan.drawItemCount, snapshot.drawItems.size());
}

TEST(AssetThumbnailBehaviorTests, PrefabPreviewResourcePlanDoesNotScanEntirePrefabWhenBudgetedMeshesArePending)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async mesh request state.";
#else
    using namespace NLS::Core::ResourceManagement;
    using namespace NLS::Editor::Assets;

    ResetThumbnailPerformanceJobSystem();
    const auto root = MakeThumbnailPerformanceRoot();
    const ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");

    PreviewRenderableSnapshot snapshot;
    for (size_t index = 0u; index < 32u; ++index)
    {
        const auto meshPath = root / "Assets" / ("mesh-" + std::to_string(index) + ".nmesh");
        WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

        PreviewDrawItem item;
        item.meshPath = meshPath.generic_string();
        item.materialPaths = {
            "Library/Artifacts/material-" + std::to_string(index) + "/surface.nmat"
        };
        snapshot.drawItems.push_back(std::move(item));
    }
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();

    AssetThumbnailRequest request;
    request.kind = AssetThumbnailKind::PrefabPreview;

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    MeshManager meshManager;
    MaterialManager materialManager;
    for (size_t index = 0u; index < 4u; ++index)
        EXPECT_EQ(meshManager.RequestAsyncArtifact(snapshot.drawItems[index].meshPath, true), nullptr);

    const auto plan = BuildThumbnailPreviewPrefabResourcePlanWithManagersForTesting(
        request,
        snapshot,
        meshManager,
        materialManager,
        4u);

    EXPECT_TRUE(plan.truncatedForPendingResources)
        << "Already queued pending mesh artifacts should still stop the current prefab resource-planning slice.";
    EXPECT_LE(plan.uniqueMeshLoadPathCount, 4u)
        << "A heavy GPU pump with four pending mesh requests must not scan the rest of a large prefab on the UI thread.";
    EXPECT_LT(plan.drawItemCount, snapshot.drawItems.size());

    MeshManager::ClearAsyncArtifactRequestStateForTesting();
    meshManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, PreviewRenderableSnapshotIncludesPathOnlyRendererDependencies)
{
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Serialize;

    auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto secondGameObjectId = MakeObjectId("91919191-9191-4191-8191-919191919191");
    const auto secondMeshFilterId = MakeObjectId("92929292-9292-4292-8292-929292929292");
    const auto secondMeshRendererId = MakeObjectId("93939393-9393-4393-8393-939393939393");
    const auto secondMeshId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("94949494-9494-4494-8494-949494949494"));
    const auto secondMaterialId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("95959595-9595-4595-8595-959595959595"));

    prefab.graph.objects.push_back(ObjectRecord{
        secondGameObjectId,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
        "PathOnlyRenderer",
        "PathOnlyRenderer",
        ObjectRecordState::Alive,
        {
            {
                "components",
                PropertyValue::Array({
                    PropertyValue::OwnedReference(secondMeshFilterId),
                    PropertyValue::OwnedReference(secondMeshRendererId)
                })
            }
        }});
    prefab.graph.objects.push_back(ObjectRecord{
        secondMeshFilterId,
        NLS_TYPEOF(NLS::Engine::Components::MeshFilter).GetName(),
        "PathOnlyMeshFilter",
        "PathOnlyRenderer/MeshFilter",
        ObjectRecordState::Alive,
        {
            {
                "mesh",
                PropertyValue::String("Library/Artifacts/94949494-9494-4494-8494-949494949494/PathOnly.nmesh")
            }
        }});
    prefab.graph.objects.push_back(ObjectRecord{
        secondMeshRendererId,
        NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName(),
        "PathOnlyMeshRenderer",
        "PathOnlyRenderer/MeshRenderer",
        ObjectRecordState::Alive,
        {
            {
                "materials",
                PropertyValue::Array({
                    PropertyValue::String("Library/Artifacts/95959595-9595-4595-8595-959595959595/PathOnly.nmat")
                })
            }
        }});
    prefab.resolvedAssets.push_back({
        secondMeshId,
        "Mesh",
        "mesh:PathOnly",
        "Library/Artifacts/94949494-9494-4494-8494-949494949494/PathOnly.nmesh"
    });
    prefab.resolvedAssets.push_back({
        secondMaterialId,
        "Material",
        "material:PathOnly",
        "Library/Artifacts/95959595-9595-4595-8595-959595959595/PathOnly.nmat"
    });

    const auto snapshot = BuildPreviewRenderableSnapshot(prefab);

    ASSERT_EQ(snapshot.drawItems.size(), 2u);
    EXPECT_EQ(
        snapshot.drawItems[1].meshPath,
        "Library/Artifacts/94949494-9494-4494-8494-949494949494/PathOnly.nmesh");
    ASSERT_EQ(snapshot.drawItems[1].materialPaths.size(), 1u);
    EXPECT_EQ(
        snapshot.drawItems[1].materialPaths.front(),
        "Library/Artifacts/95959595-9595-4595-8595-959595959595/PathOnly.nmat");
    EXPECT_EQ(snapshot.drawItems[1].meshAssetId, secondMeshId);
    ASSERT_EQ(snapshot.drawItems[1].materialAssetIds.size(), 1u);
    EXPECT_EQ(snapshot.drawItems[1].materialAssetIds.front(), secondMaterialId);
}

TEST(AssetThumbnailBehaviorTests, PreviewRenderableSnapshotFlattensParentTransformsForChildRenderers)
{
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Serialize;

    auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto parentId = MakeObjectId("a0a0a0a0-a0a0-40a0-80a0-a0a0a0a0a0a0");
    const auto childId = prefab.graph.root;

    prefab.graph.root = parentId;
    auto& child = prefab.graph.objects.front();
    child.properties.push_back({
        "parent",
        PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(
            MakeLocalIdentifierInFile(parentId)))
    });

    ObjectRecord parent {
        parentId,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
        "PreviewParent",
        "PreviewParent",
        ObjectRecordState::Alive,
        {
            {
                "children",
                PropertyValue::Array({PropertyValue::OwnedReference(childId)})
            },
            {
                "components",
                PropertyValue::Array({})
            },
            {
                "parent",
                PropertyValue::Null()
            },
            MakePreviewTransformProperty(10.0, 0.0, 0.0, 2.0, 2.0, 2.0)
        },
        MakeLocalIdentifierInFile(parentId)
    };
    prefab.graph.objects.push_back(std::move(parent));

    const auto snapshot = BuildPreviewRenderableSnapshot(prefab);

    ASSERT_EQ(snapshot.drawItems.size(), 1u);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.x, 16.0f);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.y, 8.0f);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.z, 10.0f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.x, 4.0f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.y, 5.0f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.z, 6.0f);
}

TEST(AssetThumbnailBehaviorTests, PreviewRenderableSnapshotReadsImportedTransformComponents)
{
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Serialize;

    auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    auto& gameObject = prefab.graph.objects.front();
    gameObject.properties.erase(
        std::remove_if(
            gameObject.properties.begin(),
            gameObject.properties.end(),
            [](const PropertyRecord& property)
            {
                return property.name == "m_transform";
            }),
        gameObject.properties.end());

    const auto transformId = MakeObjectId("b0b0b0b0-b0b0-40b0-80b0-b0b0b0b0b0b0");
    for (auto& property : gameObject.properties)
    {
        if (property.name != "components" ||
            property.value.GetKind() != PropertyValue::Kind::Array)
        {
            continue;
        }

        auto components = property.value.GetArray();
        components.insert(components.begin(), PropertyValue::OwnedReference(transformId));
        property.value = PropertyValue::Array(std::move(components));
    }

    prefab.graph.objects.push_back(ObjectRecord{
        transformId,
        NLS_TYPEOF(NLS::Engine::Components::TransformComponent).GetName(),
        "PreviewRoot Transform",
        "PreviewRoot/Transform",
        ObjectRecordState::Alive,
        {
            {"localPosition", MakePreviewVector3Value(7.0, 8.0, 9.0)},
            {"localRotation", MakePreviewQuaternionValue(0.0, 0.38268343, 0.0, 0.9238795)},
            {"localScale", MakePreviewVector3Value(1.5, 2.0, 2.5)}
        },
        MakeLocalIdentifierInFile(transformId)
    });

    const auto snapshot = BuildPreviewRenderableSnapshot(prefab);

    ASSERT_EQ(snapshot.drawItems.size(), 1u);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.x, 7.0f);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.y, 8.0f);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.z, 9.0f);
    EXPECT_NEAR(snapshot.drawItems.front().localRotation.x, 0.0f, 0.0001f);
    EXPECT_NEAR(snapshot.drawItems.front().localRotation.y, 0.38268343f, 0.0001f);
    EXPECT_NEAR(snapshot.drawItems.front().localRotation.z, 0.0f, 0.0001f);
    EXPECT_NEAR(snapshot.drawItems.front().localRotation.w, 0.9238795f, 0.0001f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.x, 1.5f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.y, 2.0f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.z, 2.5f);
}

TEST(AssetThumbnailBehaviorTests, PreviewRenderableSnapshotFlattensImportedTransformComponentHierarchy)
{
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Serialize;

    auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto parentId = MakeObjectId("c0c0c0c0-c0c0-40c0-80c0-c0c0c0c0c0c0");
    const auto parentTransformId = MakeObjectId("c1c1c1c1-c1c1-40c1-80c1-c1c1c1c1c1c1");
    const auto childTransformId = MakeObjectId("c2c2c2c2-c2c2-40c2-80c2-c2c2c2c2c2c2");
    const auto childId = prefab.graph.root;

    prefab.graph.root = parentId;
    auto& child = prefab.graph.objects.front();
    child.properties.erase(
        std::remove_if(
            child.properties.begin(),
            child.properties.end(),
            [](const PropertyRecord& property)
            {
                return property.name == "m_transform";
            }),
        child.properties.end());
    child.properties.push_back({
        "parent",
        PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(
            MakeLocalIdentifierInFile(parentId)))
    });
    for (auto& property : child.properties)
    {
        if (property.name != "components" ||
            property.value.GetKind() != PropertyValue::Kind::Array)
        {
            continue;
        }

        auto components = property.value.GetArray();
        components.insert(components.begin(), PropertyValue::OwnedReference(childTransformId));
        property.value = PropertyValue::Array(std::move(components));
    }

    prefab.graph.objects.push_back(ObjectRecord{
        parentId,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
        "PreviewParent",
        "PreviewParent",
        ObjectRecordState::Alive,
        {
            {
                "children",
                PropertyValue::Array({PropertyValue::OwnedReference(childId)})
            },
            {
                "components",
                PropertyValue::Array({PropertyValue::OwnedReference(parentTransformId)})
            },
            {
                "parent",
                PropertyValue::Null()
            }
        },
        MakeLocalIdentifierInFile(parentId)
    });
    prefab.graph.objects.push_back(ObjectRecord{
        parentTransformId,
        NLS_TYPEOF(NLS::Engine::Components::TransformComponent).GetName(),
        "PreviewParent Transform",
        "PreviewParent/Transform",
        ObjectRecordState::Alive,
        {
            {"localPosition", MakePreviewVector3Value(10.0, 0.0, 0.0)},
            {"localRotation", MakePreviewQuaternionValue(0.0, 0.0, 0.0, 1.0)},
            {"localScale", MakePreviewVector3Value(2.0, 2.0, 2.0)}
        },
        MakeLocalIdentifierInFile(parentTransformId)
    });
    prefab.graph.objects.push_back(ObjectRecord{
        childTransformId,
        NLS_TYPEOF(NLS::Engine::Components::TransformComponent).GetName(),
        "PreviewRoot Transform",
        "PreviewRoot/Transform",
        ObjectRecordState::Alive,
        {
            {"localPosition", MakePreviewVector3Value(3.0, 4.0, 5.0)},
            {"localRotation", MakePreviewQuaternionValue(0.0, 0.0, 0.0, 1.0)},
            {"localScale", MakePreviewVector3Value(2.0, 2.5, 3.0)}
        },
        MakeLocalIdentifierInFile(childTransformId)
    });

    const auto snapshot = BuildPreviewRenderableSnapshot(prefab);

    ASSERT_EQ(snapshot.drawItems.size(), 1u);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.x, 16.0f);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.y, 8.0f);
    EXPECT_EQ(snapshot.drawItems.front().localPosition.z, 10.0f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.x, 4.0f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.y, 5.0f);
    EXPECT_EQ(snapshot.drawItems.front().localScale.z, 6.0f);
}
