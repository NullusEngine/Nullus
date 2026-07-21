#include <gtest/gtest.h>

#include "Assets/AssetThumbnailCache.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/AssetThumbnailService.h"
#include "Assets/EditorThumbnailPreviewRenderer.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PreviewRenderableSnapshot.h"
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
#include "Jobs/JobSystem.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/DriverSettings.h"
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
    explicit ScopedThumbnailPerformanceJobSystem(const uint32_t backgroundWorkerCount = 1u)
    {
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif

        NLS::Base::Jobs::JobSystemConfig config;
        config.workerCount = 0u;
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
        return result;
    }

    size_t renderCount = 0u;
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
        result.diagnostic = "thumbnail-gpu-preview-readback-pending";
        result.gpuTexture = {
            MakeOpaqueThumbnailGpuResource<NLS::Render::RHI::RHITexture>(),
            MakeOpaqueThumbnailGpuResource<NLS::Render::RHI::RHITextureView>(),
            std::make_shared<uint8_t>(0u),
            2u,
            2u
        };
        return result;
    }
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
    EXPECT_FALSE(std::filesystem::exists(generated->imagePath))
        << "Direct GPU publication must not wait for the persistence PNG.";

    std::filesystem::remove_all(root);
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

    EXPECT_FALSE(service.StartNextThumbnailGeneration())
        << "GPU-capable model and prefab previews must not enter the CPU raster worker path.";
    EXPECT_FALSE(std::filesystem::exists(gpuResult->imagePath));
    EXPECT_NE(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);

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

        const auto recovered = renderer.PumpResources(request);
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
    meshManager.RegisterResource((root / meshAArtifactPath).generic_string(), makeReadyMesh());

    EditorThumbnailPreviewResourcePumpResult mixedPump;
    for (size_t attempt = 0u;
        attempt < 128u && (meshManager.asyncRequestCount == 0u || materialManager.asyncRequestCount < 2u);
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
    EXPECT_EQ(materialManager.asyncRequestCount, 2u);
    EXPECT_TRUE(ContainsPathWithSuffix(
        materialManager.asyncRequestPaths,
        std::filesystem::path(materialAArtifactPath).filename().generic_string()));
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
    EXPECT_NE(completePump.diagnostic.find("material=2"), std::string::npos)
        << "Ready meshes must not bypass cold material dependencies and produce a white prefab preview.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailBehaviorTests, GpuPrefabPreviewRendersReadyPrefabToVisiblePixels)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto assetId = prefab.assetId;
    const auto prefabPayload = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
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
    prefab.resolvedAssets[0].artifactPath = meshArtifactPath;
    prefab.resolvedAssets[1].artifactPath = materialArtifactPath;

    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        root / prefabArtifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteBinaryFile(root / meshArtifactPath, NLS::Render::Assets::SerializeMeshArtifact(LitTriangleMeshArtifact()));
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

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
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
    auto* mesh = new NLS::Render::Resources::Mesh(
        meshArtifact.vertices,
        meshArtifact.indices,
        meshArtifact.materialIndex,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        meshArtifact.boundingSphere);
    meshManager.RegisterResource((root / meshArtifactPath).generic_string(), mesh);
    auto* shader = shaderManager.GetResource(":Shaders/StandardPBR.hlsl", true);
    ASSERT_NE(shader, nullptr);
    shader->SetImportedShaderLabPassForTesting(
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
        "shader:standardpbr/forward#thumbnail-prefab",
        "Forward",
        {});
    auto* material = new NLS::Render::Resources::Material(shader);
    material->SetShaderLabSourcePath("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader");
    material->RegisterShaderLabPassShader(shader);
    auto* whiteTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(255u, 255u, 255u, 255u);
    auto* blackTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(0u, 0u, 0u, 255u);
    ASSERT_NE(whiteTexture, nullptr);
    ASSERT_NE(blackTexture, nullptr);
    textureManager.RegisterResource(":test/thumbnail-prefab-white", whiteTexture);
    textureManager.RegisterResource(":test/thumbnail-prefab-black", blackTexture);
    material->SetRawParameter("u_Albedo", NLS::Maths::Vector4(0.72f, 0.74f, 0.78f, 1.0f));
    material->SetRawParameter("u_Metallic", 0.0f);
    material->SetRawParameter("u_Roughness", 0.72f);
    material->SetRawParameter("u_AmbientOcclusion", 1.0f);
    material->SetRawParameter("u_EnableNormalMapping", 0.0f);
    material->SetRawParameter("u_Emissive", NLS::Maths::Vector4(0.72f, 0.74f, 0.78f, 1.0f));
    material->SetRawParameter("u_Specular", NLS::Maths::Vector4(0.0f, 0.0f, 0.0f, 1.0f));
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
    materialManager.RegisterResource((root / materialArtifactPath).generic_string(), material);

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

TEST(AssetThumbnailBehaviorTests, GpuPreviewResourcePumpBudgetsKeepMaterialLoadsSingleStep)
{
    EXPECT_GE(NLS::Editor::Assets::GetThumbnailPreviewMeshPumpBudgetForTesting(), 4u);
    EXPECT_EQ(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabMeshRequestStartBudgetForTesting(),
        1u)
        << "Large prefab mesh requests must be admitted one at a time so synchronous request setup cannot monopolize the UI thread.";
    EXPECT_EQ(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabMeshPumpBudgetForTesting(),
        1u)
        << "Large prefab mesh completions must be consumed one at a time so runtime upload publication stays inside the adaptive UI budget.";
    EXPECT_EQ(NLS::Editor::Assets::GetThumbnailPreviewMaterialPumpBudgetForTesting(), 1u)
        << "Material artifact promotion can touch shader dependency registration, so thumbnail preview keeps it to one completion per frame to avoid UI spikes.";
    EXPECT_GE(NLS::Editor::Assets::GetThumbnailPreviewTexturePumpBudgetForTesting(), 4u);
    EXPECT_EQ(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabTexturePumpBudgetForTesting(),
        1u)
        << "Large prefab texture completions can decode and publish multi-megabyte images, so only one may run in an interactive frame.";
    EXPECT_GE(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabResourceInspectionBudgetForTesting(),
        32u)
        << "Large visible prefabs must discover missing mesh requests in wide cheap batches; a four-item batch combined with resource-pending cooldown takes minutes before rendering can begin.";
    EXPECT_GT(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabResourcePumpTimeBudgetMicrosForTesting(),
        0u)
        << "Count limits alone cannot protect the UI when one artifact promotion is unexpectedly expensive.";
    EXPECT_GT(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabSceneAssemblyBudgetForTesting(),
        0u);
    EXPECT_LT(
        NLS::Editor::Assets::GetThumbnailPreviewPrefabSceneAssemblyBudgetForTesting(),
        NLS::Editor::Assets::GetThumbnailPreviewPrefabDrawItemCapacityForTesting())
        << "Prefab preview scene objects must be assembled across frames so a complex thumbnail cannot monopolize the UI thread.";
}

TEST(AssetThumbnailBehaviorTests, LargePrefabWaitsForPersistentProxyBeforePumpingSourceMeshes)
{
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(ShouldWaitForPersistentPrefabPreviewProxyForTesting(true, false))
        << "The bounded source plan still references full source meshes and must not be treated as a cheap UE-style thumbnail proxy.";
    EXPECT_FALSE(ShouldWaitForPersistentPrefabPreviewProxyForTesting(true, true));
    EXPECT_FALSE(ShouldWaitForPersistentPrefabPreviewProxyForTesting(false, false));
}

TEST(AssetThumbnailBehaviorTests, FinalPrefabProxyCameraUsesRenderedProxyBounds)
{
    using namespace NLS::Editor::Assets;

    EXPECT_TRUE(ShouldUseFullSourceBoundsForPrefabCameraForTesting(true))
        << "A provisional subset should retain full-source framing so it represents the complete asset footprint.";
    EXPECT_FALSE(ShouldUseFullSourceBoundsForPrefabCameraForTesting(false))
        << "A final simplified proxy must be framed from its actual geometry; imported source spheres can be far more conservative and shrink the thumbnail to a speck.";
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

TEST(AssetThumbnailBehaviorTests, PrefabPreviewResourcePlanProxiesMoreThanLegacyDrawLimit)
{
    using namespace NLS::Editor::Assets;

    PreviewRenderableSnapshot snapshot;
    constexpr size_t kLargePrefabDrawItems = 2048u;
    snapshot.drawItems.reserve(kLargePrefabDrawItems);
    for (size_t index = 0u; index < kLargePrefabDrawItems; ++index)
    {
        PreviewDrawItem item;
        item.meshPath = "Library/Artifacts/mesh-" + std::to_string(index) + "/chunk.nmesh";
        item.localPosition = {
            static_cast<float>(index % 64u),
            static_cast<float>((index / 64u) % 8u),
            static_cast<float>(index / 512u)
        };
        snapshot.drawItems.push_back(std::move(item));
    }
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();

    AssetThumbnailRequest request;
    request.kind = AssetThumbnailKind::PrefabPreview;
    const auto plan = BuildThumbnailPreviewPrefabResourcePlanForTesting(request, snapshot);

    EXPECT_EQ(plan.drawItemCount, kLargePrefabDrawItems)
        << "Large prefabs must remain complete and rely on time-sliced preparation.";
    EXPECT_EQ(plan.dependencyDrawItemInspectionCount, kLargePrefabDrawItems);
    EXPECT_TRUE(plan.hasFullWorldBounds);
}

TEST(AssetThumbnailBehaviorTests, PrefabPreviewProxySamplesAcrossCollapsedNodeTransforms)
{
    using namespace NLS::Editor::Assets;

    PreviewRenderableSnapshot snapshot;
    constexpr size_t kDrawItemCount = 320u;
    snapshot.drawItems.reserve(kDrawItemCount);
    for (size_t index = 0u; index < kDrawItemCount; ++index)
    {
        PreviewDrawItem item;
        item.meshPath = "Library/Artifacts/mesh-" + std::to_string(index) + "/chunk.nmesh";
        snapshot.drawItems.push_back(std::move(item));
    }
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();

    AssetThumbnailRequest request;
    request.kind = AssetThumbnailKind::PrefabPreview;
    const auto plan = BuildThumbnailPreviewPrefabResourcePlanForTesting(request, snapshot);

    ASSERT_EQ(plan.selectedDrawItemIndices.size(), kDrawItemCount);
    EXPECT_EQ(plan.dependencyDrawItemInspectionCount, kDrawItemCount);
    EXPECT_EQ(plan.selectedDrawItemIndices.front(), 0u);
    EXPECT_GT(plan.selectedDrawItemIndices.back(), kDrawItemCount * 9u / 10u)
        << "Collapsed transform bounds must not make a large imported model use only its first meshes.";
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

TEST(AssetThumbnailBehaviorTests, PersistentPrefabPreviewProxyCoversEverySourceInstanceAndReusesCache)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to build the persistent thumbnail proxy.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    struct ScopedRuntimeArtifactAuthorization
    {
        bool previousEnabled = NLS::Core::Assets::IsRuntimeArtifactAuthorizationEnabled();

        ScopedRuntimeArtifactAuthorization()
        {
            NLS::Core::Assets::ClearRuntimeArtifactAuthorization();
            NLS::Core::Assets::SetRuntimeArtifactAuthorizationEnabled(true);
        }

        ~ScopedRuntimeArtifactAuthorization()
        {
            NLS::Core::Assets::ClearRuntimeArtifactAuthorization();
            NLS::Core::Assets::SetRuntimeArtifactAuthorizationEnabled(previousEnabled);
        }
    } authorization;
    const auto meshPath = root / "Library" / "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(
            "aa0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcd");
    WriteBinaryFile(meshPath, NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    PreviewRenderableSnapshot snapshot;
    constexpr size_t kInstanceCount = 65u;
    snapshot.drawItems.reserve(kInstanceCount);
    for (size_t index = 0u; index < kInstanceCount; ++index)
    {
        PreviewDrawItem item;
        item.meshPath = meshPath.generic_string();
        item.localPosition = {static_cast<float>(index) * 2.0f, 0.0f, 0.0f};
        snapshot.drawItems.push_back(std::move(item));
    }
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a1a1a1a1-b2b2-4c3c-8d4d-e5e5e5e5e5e5"));
    request.sourceAssetPath = "Assets/Large.prefab";
    request.subAssetKey = "prefab:Large";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.previewRendererVersion = "persistent-proxy-test:v1";
    request.dependencyStamp = "source-revision:1";

    const auto expectedPath = BuildThumbnailPreviewPrefabProxyArtifactPathForTesting(request);
    const auto first = BuildThumbnailPreviewPrefabProxyForTesting(request, snapshot);
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(
        NLS::Core::Assets::TryMakePortableContentArtifactPath(meshPath.generic_string())))
        << "The editor proxy builder must authorize only its validated content-addressed source artifact.";
    EXPECT_EQ(*first, expectedPath);
    const auto proxy = NLS::Render::Assets::LoadMeshArtifact(*first);
    ASSERT_TRUE(proxy.has_value());
    ASSERT_GE(proxy->indices.size(), kInstanceCount * 3u)
        << "The proxy must include geometry from every source instance rather than selecting a child subset.";

    float minimumX = std::numeric_limits<float>::max();
    float maximumX = std::numeric_limits<float>::lowest();
    for (const auto& vertex : proxy->vertices)
    {
        minimumX = (std::min)(minimumX, vertex.position[0]);
        maximumX = (std::max)(maximumX, vertex.position[0]);
    }
    EXPECT_LT(minimumX, 1.0f);
    EXPECT_GT(maximumX, 126.0f);

    const auto second = BuildThumbnailPreviewPrefabProxyForTesting(request, snapshot);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, *first)
        << "An unchanged prefab must reuse its persistent proxy artifact.";

    auto changedRequest = request;
    changedRequest.dependencyStamp = "source-revision:2";
    EXPECT_NE(
        BuildThumbnailPreviewPrefabProxyArtifactPathForTesting(changedRequest),
        *first)
        << "Source dependency changes must invalidate the persistent proxy identity.";

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailBehaviorTests, PersistentPrefabPreviewProxyPreservesMaterialGroups)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to build the persistent thumbnail proxy.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto stoneMeshPath = root / "Library" / "Artifacts" / "aa" / "stone.nmesh";
    const auto metalMeshPath = root / "Library" / "Artifacts" / "bb" / "metal.nmesh";
    auto stoneMesh = TriangleMeshArtifact();
    stoneMesh.materialIndex = 0u;
    auto metalMesh = TriangleMeshArtifact();
    metalMesh.materialIndex = 1u;
    WriteBinaryFile(stoneMeshPath, NLS::Render::Assets::SerializeMeshArtifact(stoneMesh));
    WriteBinaryFile(metalMeshPath, NLS::Render::Assets::SerializeMeshArtifact(metalMesh));

    const std::string stoneMaterial = "Assets/Materials/Stone.nmat";
    const std::string metalMaterial = "Assets/Materials/Metal.nmat";
    PreviewRenderableSnapshot snapshot;
    constexpr size_t kInstanceCount = 66u;
    snapshot.drawItems.reserve(kInstanceCount);
    for (size_t index = 0u; index < kInstanceCount; ++index)
    {
        PreviewDrawItem item;
        item.meshPath = index % 2u == 0u
            ? stoneMeshPath.generic_string()
            : metalMeshPath.generic_string();
        item.materialPaths = {stoneMaterial, metalMaterial};
        item.localPosition = {static_cast<float>(index), 0.0f, 0.0f};
        snapshot.drawItems.push_back(std::move(item));
    }
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("b2b2b2b2-c3c3-4d4d-8e5e-f6f6f6f6f6f6"));
    request.sourceAssetPath = "Assets/MaterialGroups.prefab";
    request.subAssetKey = "prefab:MaterialGroups";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.previewRendererVersion = "persistent-proxy-material-test:v1";
    request.dependencyStamp = "source-revision:1";

    const auto proxy = BuildThumbnailPreviewPrefabProxyDetailsForTesting(request, snapshot);
    ASSERT_TRUE(proxy.has_value());
    ASSERT_EQ(proxy->meshPaths.size(), 2u);
    ASSERT_EQ(proxy->materialPaths.size(), proxy->meshPaths.size());
    EXPECT_NE(
        std::find(proxy->materialPaths.begin(), proxy->materialPaths.end(), stoneMaterial),
        proxy->materialPaths.end());
    EXPECT_NE(
        std::find(proxy->materialPaths.begin(), proxy->materialPaths.end(), metalMaterial),
        proxy->materialPaths.end());

    size_t proxyIndexCount = 0u;
    for (const auto& path : proxy->meshPaths)
    {
        const auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(path);
        ASSERT_TRUE(meshArtifact.has_value());
        EXPECT_EQ(meshArtifact->materialIndex, 0u)
            << "Each material-group proxy exposes its source material as slot zero.";
        proxyIndexCount += meshArtifact->indices.size();
    }
    EXPECT_GE(proxyIndexCount, kInstanceCount * 3u)
        << "Material grouping must not drop source instances from the proxy.";

    std::filesystem::remove_all(root);
#endif
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
