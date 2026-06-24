#include <gtest/gtest.h>

#include "Assets/AssetThumbnailCache.h"
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
#include "Jobs/JobSystem.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphWriter.h"

#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
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

class ScopedThumbnailResourceManagerAssetPaths final
{
public:
    ScopedThumbnailResourceManagerAssetPaths(
        const std::filesystem::path& projectAssetsRoot,
        const std::filesystem::path& engineAssetsRoot)
    {
        NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
            projectAssetsRoot.string(),
            engineAssetsRoot.string());
    }

    ~ScopedThumbnailResourceManagerAssetPaths()
    {
        NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths({}, {});
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

PerformanceBenchmarkRun MakeThumbnailBenchmarkRun(
    std::string scenarioName,
    const PerformanceStageStatsSnapshot& snapshot,
    std::chrono::microseconds totalDuration = std::chrono::microseconds{1})
{
    PerformanceBenchmarkRun run;
    run.scenarioName = std::move(scenarioName);
    run.runType = PerformanceBenchmarkRunType::Baseline;
    run.totalDuration = totalDuration;
    run.stageStats = snapshot;
    return run;
}

void WriteThumbnailPerformanceReportIfRequested(
    const std::string& scenarioName,
    const PerformanceStageStatsSnapshot& snapshot,
    std::chrono::microseconds totalDuration)
{
    const auto* reportDirectory = std::getenv("NLS_PERFORMANCE_REPORT_DIR");
    if (reportDirectory == nullptr || std::string(reportDirectory).empty())
        return;

    std::filesystem::create_directories(reportDirectory);
    PerformanceBenchmarkRun run;
    run.scenarioName = scenarioName;
    run.runType = PerformanceBenchmarkRunType::Baseline;
    run.totalDuration = totalDuration;
    run.stageStats = snapshot;

    std::ofstream output(
        std::filesystem::path(reportDirectory) / (scenarioName + ".txt"),
        std::ios::binary | std::ios::trunc);
    output << FormatPerformanceBenchmarkReport(run);
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
    const std::string& scenarioName,
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

    const auto snapshot = stats.Snapshot();
    WriteThumbnailPerformanceReportIfRequested(scenarioName, snapshot, *scenarioElapsed);
    return snapshot;
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

TEST(AssetThumbnailPerformanceTests, TextureThumbnailQueueAndGenerationEmitDiagnosticStages)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);

    const auto scenarioBegin = std::chrono::steady_clock::now();
    const auto request = MakeTextureRequest(root);
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail();
    const auto scenarioElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - scenarioBegin);
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

    WriteThumbnailPerformanceReportIfRequested(
        "Thumbnail_TextureFirstGeneration",
        snapshot,
        scenarioElapsed);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailPerformanceTests, DiskCacheHitReportsLookupHitWithoutRegeneration)
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
        "Thumbnail_DiskCacheHit",
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

TEST(AssetThumbnailPerformanceTests, MemoryCacheHitReportsLookupHitWithoutRegeneration)
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

    WriteThumbnailPerformanceReportIfRequested(
        "Thumbnail_MemoryCacheHit",
        snapshot,
        std::chrono::microseconds{1});

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailPerformanceTests, RapidScrollDuplicateRequestsReportBacklogAndCoalescingPressure)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    constexpr size_t uniqueRequestCount = 32u;
    constexpr size_t duplicateRequestCount = 500u;
    for (size_t index = 0; index < uniqueRequestCount; ++index)
        WriteBinaryFile(root / "Assets" / "Textures" / ("Hero" + std::to_string(index) + ".png"), TinyPng());

    AssetThumbnailService service;
    ThumbnailGenerationBudget budget;
    budget.cacheWriteCountBudget = 0u;
    service.SetThumbnailGenerationBudget(budget);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    for (size_t index = 0; index < duplicateRequestCount; ++index)
    {
        const auto request = MakeTextureRequestForIndex(root, index % uniqueRequestCount);
        EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    }
    const auto generated = service.GenerateNextThumbnail();
    EXPECT_FALSE(generated.has_value());

    const auto snapshot = stats.Snapshot();
    const auto* lookup = FindThumbnailStage(snapshot, "ThumbnailCacheLookup");
    ASSERT_NE(lookup, nullptr);
    ASSERT_TRUE(lookup->counters.contains("duplicateThumbnailRequestCount"));
    EXPECT_EQ(lookup->counters.at("duplicateThumbnailRequestCount"), duplicateRequestCount - uniqueRequestCount);
    ASSERT_TRUE(lookup->counters.contains("coalescingPressure"));
    EXPECT_EQ(lookup->counters.at("coalescingPressure"), duplicateRequestCount - uniqueRequestCount);
    ASSERT_TRUE(lookup->counters.contains("queueDepth"));
    EXPECT_EQ(lookup->counters.at("queueDepth"), uniqueRequestCount);

    const auto* total = FindThumbnailStage(snapshot, "TotalThumbnail");
    ASSERT_NE(total, nullptr);
    ASSERT_TRUE(total->counters.contains("queueBacklog"));
    EXPECT_EQ(total->counters.at("queueBacklog"), uniqueRequestCount);
    ASSERT_TRUE(total->counters.contains("inFlightRequestCount"));
    EXPECT_EQ(total->counters.at("inFlightRequestCount"), 0u);

    WriteThumbnailPerformanceReportIfRequested(
        "Thumbnail_RapidScrollDeduplication",
        snapshot,
        std::chrono::microseconds{1});

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailPerformanceTests, RapidScrollBenchmarkHonorsCacheWriteBudgetPerFrame)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    constexpr size_t uniqueRequestCount = 4u;
    constexpr size_t duplicateRequestCount = 40u;
    for (size_t index = 0; index < uniqueRequestCount; ++index)
        WriteBinaryFile(root / "Assets" / "Textures" / ("Hero" + std::to_string(index) + ".png"), TinyPng());

    AssetThumbnailService service;
    ThumbnailGenerationBudget budget;
    budget.cacheWriteCountBudget = 1u;
    service.SetThumbnailGenerationBudget(budget);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    for (size_t index = 0; index < duplicateRequestCount; ++index)
    {
        const auto request = MakeTextureRequestForIndex(root, index % uniqueRequestCount);
        ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    }
    ASSERT_EQ(service.GetQueuedRequestCount(), uniqueRequestCount);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetQueuedRequestCount(), uniqueRequestCount - 1u);

    const auto budgetExhausted = service.GenerateNextThumbnail();
    EXPECT_FALSE(budgetExhausted.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), uniqueRequestCount - 1u);

    const auto snapshot = stats.Snapshot();
    const auto* lookup = FindThumbnailStage(snapshot, "ThumbnailCacheLookup");
    ASSERT_NE(lookup, nullptr);
    ASSERT_TRUE(lookup->counters.contains("duplicateThumbnailRequestCount"));
    EXPECT_EQ(lookup->counters.at("duplicateThumbnailRequestCount"), duplicateRequestCount - uniqueRequestCount);
    ASSERT_TRUE(lookup->counters.contains("coalescingPressure"));
    EXPECT_EQ(lookup->counters.at("coalescingPressure"), duplicateRequestCount - uniqueRequestCount);

    const auto* total = FindThumbnailStage(snapshot, "TotalThumbnail");
    ASSERT_NE(total, nullptr);
    ASSERT_TRUE(total->counters.contains("thumbnailsGeneratedThisFrame"));
    EXPECT_EQ(total->counters.at("thumbnailsGeneratedThisFrame"), 1u);
    ASSERT_TRUE(total->counters.contains("cacheWriteBudgetRemaining"));
    EXPECT_EQ(total->counters.at("cacheWriteBudgetRemaining"), 0u);
    ASSERT_TRUE(total->counters.contains("queueBacklog"));
    EXPECT_GE(total->counters.at("queueBacklog"), uniqueRequestCount - 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailPerformanceTests, ThumbnailGenerationBudgetTracksCpuPreparationAndGpuUploadBytes)
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

TEST(AssetThumbnailPerformanceTests, SupersededQueuedRequestsReportCancellationDiagnostics)
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

TEST(AssetThumbnailPerformanceTests, ThumbnailReportIncludesTopFiveAndSchedulerCounters)
{
    using namespace NLS::Editor::Assets;

    PerformanceStageStats stats;
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "TotalThumbnail",
        PerformanceStageThread::Main,
        std::chrono::microseconds{700},
        {
            {"thumbnailsGeneratedThisFrame", 1u},
            {"queueBacklog", 12u},
            {"inFlightRequestCount", 3u},
            {"cancellationLatency", 25u}
        }
    });
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "ThumbnailCacheLookup",
        PerformanceStageThread::Main,
        std::chrono::microseconds{250},
        {
            {"duplicateThumbnailRequestCount", 5u},
            {"queueDepth", 8u},
            {"coalescingPressure", 4u}
        }
    });
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "WaitPreviewFence",
        PerformanceStageThread::Main,
        std::chrono::microseconds{500},
        {{"fenceWaitTime", 500u}}
    });
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "EncodePreview",
        PerformanceStageThread::Background,
        std::chrono::microseconds{150},
        {{"encodedByteCount", 64u}}
    });
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "StorePreviewCache",
        PerformanceStageThread::Background,
        std::chrono::microseconds{100},
        {{"cacheWriteCount", 1u}}
    });

    const auto snapshot = stats.Snapshot();
    const auto topBottlenecks = stats.TopBottlenecks(PerformanceStageDomain::Thumbnail, 5u);
    ASSERT_EQ(topBottlenecks.size(), 5u);
    EXPECT_EQ(topBottlenecks[0].stageName, "TotalThumbnail");
    EXPECT_EQ(topBottlenecks[1].stageName, "WaitPreviewFence");
    EXPECT_EQ(topBottlenecks[2].stageName, "ThumbnailCacheLookup");
    EXPECT_EQ(topBottlenecks[3].stageName, "EncodePreview");
    EXPECT_EQ(topBottlenecks[4].stageName, "StorePreviewCache");

    auto run = MakeThumbnailBenchmarkRun("Thumbnail_ReportCounters", snapshot);
    const auto report = FormatPerformanceBenchmarkReport(run, 5u);

    EXPECT_NE(report.find("TopBottlenecks:"), std::string::npos);
    EXPECT_NE(report.find("TotalThumbnail"), std::string::npos);
    EXPECT_NE(report.find("WaitPreviewFence"), std::string::npos);
    EXPECT_NE(report.find("ThumbnailCacheLookup"), std::string::npos);
    EXPECT_NE(report.find("thumbnailsGeneratedThisFrame=1"), std::string::npos);
    EXPECT_NE(report.find("duplicateThumbnailRequestCount=5"), std::string::npos);
    EXPECT_NE(report.find("queueDepth=8"), std::string::npos);
    EXPECT_NE(report.find("queueBacklog=12"), std::string::npos);
    EXPECT_NE(report.find("inFlightRequestCount=3"), std::string::npos);
    EXPECT_NE(report.find("cancellationLatency=25"), std::string::npos);
    EXPECT_NE(report.find("coalescingPressure=4"), std::string::npos);
    EXPECT_NE(report.find("fenceWaitTime=500"), std::string::npos);
}

TEST(AssetThumbnailPerformanceTests, ZeroCacheWriteBudgetDefersQueuedTextureThumbnail)
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

TEST(AssetThumbnailPerformanceTests, ZeroCacheWriteBudgetDoesNotStartAsyncTextureThumbnail)
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

TEST(AssetThumbnailPerformanceTests, AsyncTextureGenerationRecordsEncodeAndStoreAsBackgroundWork)
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

TEST(AssetThumbnailPerformanceTests, GpuPreviewCacheWriteRunsAsBackgroundWorkAfterReadback)
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

TEST(AssetThumbnailPerformanceTests, GpuPreviewRejectsFullyTransparentReadbackEvenWhenRgbVaries)
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

TEST(AssetThumbnailPerformanceTests, GpuPreviewRendererDoesNotSynchronouslyLoadUncachedMeshArtifact)
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
    EXPECT_EQ(rendered.diagnostic, "thumbnail-gpu-preview-resources-pending");

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

TEST(AssetThumbnailPerformanceTests, GpuPreviewRendererDoesNotSynchronouslyLoadUncachedMaterialArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("32323232-3232-4232-8232-323232323232"));
    const auto materialPath = root / "Library" / "Artifacts" / assetId.ToString() / "materials" / "Body.nmat";
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "</root>\n");

    NLS::Core::ResourceManagement::MeshManager meshManager;
    CountingMaterialManager materialManager;
    ScopedServiceOverride meshManagerOverride(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerOverride(materialManager);
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.subAssetKey = "material:Body";
    request.artifactPath = "Library/Artifacts/" + assetId.ToString() + "/materials/Body.nmat";
    request.kind = AssetThumbnailKind::MaterialSphere;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:no-sync-material-load";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {{"artifact", "uncached-material:v1"}};

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    EditorThumbnailPreviewRenderer renderer(EnsureThumbnailPerformanceTestDriver());
    const auto rendered = renderer.Render(request);

    EXPECT_TRUE(rendered.rgbaPixels.empty());
    EXPECT_EQ(rendered.diagnostic, "thumbnail-gpu-preview-resources-pending");
    EXPECT_EQ(materialManager.prewarmWithDependenciesCount, 0u);
    EXPECT_EQ(materialManager.asyncRequestCount, 1u);
    EXPECT_NE(materialManager.lastAsyncPath.find("Body.nmat"), std::string::npos);

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
            "materials/Body.nmat"),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize,
            "materials/Body.nmat"),
        0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailPerformanceTests, GpuPrefabPreviewDoesNotSynchronouslyPrewarmUncachedMeshArtifact)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto assetId = prefab.assetId;
    const auto prefabPayload = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    const auto meshId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("50505050-5050-4050-8050-505050505050"));
    const auto materialId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("60606060-6060-4060-8060-606060606060"));
    std::filesystem::create_directories(artifactRoot / "meshes");
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Hero.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nmat",
        ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "</root>\n");
    {
        std::ofstream manifest(artifactRoot / "manifest.json", std::ios::binary | std::ios::trunc);
        manifest <<
            "{"
            "\"sourceAssetId\":\"" << assetId.GetGuid().ToString() << "\","
            "\"importerId\":\"scene-model\","
            "\"importerVersion\":1,"
            "\"targetPlatform\":\"editor\","
            "\"primarySubAssetKey\":\"prefab:Hero\","
            "\"subAssets\":["
            "{"
            "\"sourceAssetId\":\"" << assetId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"prefab:Hero\","
            "\"artifactType\":\"Prefab\","
            "\"loaderId\":\"native-prefab\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"Hero.nprefab\","
            "\"contentHash\":\"prefab-hash\""
            "},"
            "{"
            "\"sourceAssetId\":\"" << meshId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"mesh:Hero\","
            "\"artifactType\":\"Mesh\","
            "\"loaderId\":\"mesh\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"meshes/Hero.nmesh\","
            "\"contentHash\":\"mesh-hash\""
            "},"
            "{"
            "\"sourceAssetId\":\"" << materialId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"material:Hero\","
            "\"artifactType\":\"Material\","
            "\"loaderId\":\"native-material\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"Hero.nmat\","
            "\"contentHash\":\"material-hash\""
            "}"
            "]"
            "}";
    }

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
    request.artifactPath = "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:no-sync-prefab-mesh-load";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {{"artifact", "uncached-prefab-mesh:v1"}};

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    EditorThumbnailPreviewRenderer renderer(EnsureThumbnailPerformanceTestDriver());
    const auto rendered = renderer.Render(request);

    EXPECT_TRUE(rendered.rgbaPixels.empty());
    EXPECT_EQ(rendered.diagnostic, "thumbnail-gpu-preview-resources-pending");
    EXPECT_EQ(meshManager.prewarmCount, 0u);
    EXPECT_EQ(meshManager.asyncRequestCount, 1u);
    EXPECT_NE(meshManager.lastAsyncPath.find("Hero.nmesh"), std::string::npos);

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
            "meshes/Hero.nmesh"),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize,
            "meshes/Hero.nmesh"),
        0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailPerformanceTests, GpuPrefabPreviewReusesSnapshotWhileResourcesArePending)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto prefab = MakePrefabArtifactWithPreviewRendererDependencies();
    const auto assetId = prefab.assetId;
    const auto prefabPayload = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    const auto meshId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("50505050-5050-4050-8050-505050505050"));
    const auto materialId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("60606060-6060-4060-8060-606060606060"));
    std::filesystem::create_directories(artifactRoot / "meshes");
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Hero.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nmat",
        ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "</root>\n");
    {
        std::ofstream manifest(artifactRoot / "manifest.json", std::ios::binary | std::ios::trunc);
        manifest <<
            "{"
            "\"sourceAssetId\":\"" << assetId.GetGuid().ToString() << "\","
            "\"importerId\":\"scene-model\","
            "\"importerVersion\":1,"
            "\"targetPlatform\":\"editor\","
            "\"primarySubAssetKey\":\"prefab:Hero\","
            "\"subAssets\":["
            "{"
            "\"sourceAssetId\":\"" << assetId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"prefab:Hero\","
            "\"artifactType\":\"Prefab\","
            "\"loaderId\":\"native-prefab\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"Hero.nprefab\","
            "\"contentHash\":\"prefab-hash\""
            "},"
            "{"
            "\"sourceAssetId\":\"" << meshId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"mesh:Hero\","
            "\"artifactType\":\"Mesh\","
            "\"loaderId\":\"mesh\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"meshes/Hero.nmesh\","
            "\"contentHash\":\"mesh-hash\""
            "},"
            "{"
            "\"sourceAssetId\":\"" << materialId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"material:Hero\","
            "\"artifactType\":\"Material\","
            "\"loaderId\":\"native-material\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"Hero.nmat\","
            "\"contentHash\":\"material-hash\""
            "}"
            "]"
            "}";
    }

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
    request.artifactPath = "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:snapshot-cache-pending";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {
        {"artifact", "snapshot-cache:v1"},
        {"dependency", "mesh-material:v1"}
    };

    EditorThumbnailPreviewRenderer renderer(EnsureThumbnailPerformanceTestDriver());
    const auto first = renderer.Render(request);
    ASSERT_TRUE(first.rgbaPixels.empty());
    ASSERT_EQ(first.diagnostic, "thumbnail-gpu-preview-resources-pending");

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    std::filesystem::remove(artifactRoot / "Hero.nprefab");

    const auto second = renderer.Render(request);

    EXPECT_TRUE(second.rgbaPixels.empty());
    EXPECT_EQ(second.diagnostic, "thumbnail-gpu-preview-resources-pending");
    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
            "Hero.nprefab"),
        0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailPerformanceTests, GpuPrefabPreviewQueuesAllMissingResourcesBeforeReturningPending)
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

    prefab.resolvedAssets[1].artifactPath =
        "Library/Artifacts/" + materialAId.ToString() + "/HeroA.nmat";

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
        "Library/Artifacts/" + meshBId.ToString() + "/HeroB.nmesh"
    });
    prefab.resolvedAssets.push_back({
        materialBId,
        "Material",
        "material:HeroB",
        "Library/Artifacts/" + materialBId.ToString() + "/HeroB.nmat"
    });

    const auto prefabPayload = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    {
        std::ofstream manifest(artifactRoot / "manifest.json", std::ios::binary | std::ios::trunc);
        manifest <<
            "{"
            "\"sourceAssetId\":\"" << assetId.GetGuid().ToString() << "\","
            "\"importerId\":\"scene-model\","
            "\"importerVersion\":1,"
            "\"targetPlatform\":\"editor\","
            "\"primarySubAssetKey\":\"prefab:Hero\","
            "\"subAssets\":["
            "{"
            "\"sourceAssetId\":\"" << assetId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"prefab:Hero\","
            "\"artifactType\":\"Prefab\","
            "\"loaderId\":\"native-prefab\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"Hero.nprefab\","
            "\"contentHash\":\"prefab-hash\""
            "},"
            "{"
            "\"sourceAssetId\":\"" << meshAId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"mesh:Hero\","
            "\"artifactType\":\"Mesh\","
            "\"loaderId\":\"mesh\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"HeroA.nmesh\","
            "\"contentHash\":\"mesh-a-hash\""
            "},"
            "{"
            "\"sourceAssetId\":\"" << materialAId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"material:Hero\","
            "\"artifactType\":\"Material\","
            "\"loaderId\":\"native-material\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"HeroA.nmat\","
            "\"contentHash\":\"material-a-hash\""
            "},"
            "{"
            "\"sourceAssetId\":\"" << meshBId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"mesh:HeroB\","
            "\"artifactType\":\"Mesh\","
            "\"loaderId\":\"mesh\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"HeroB.nmesh\","
            "\"contentHash\":\"mesh-b-hash\""
            "},"
            "{"
            "\"sourceAssetId\":\"" << materialBId.GetGuid().ToString() << "\","
            "\"subAssetKey\":\"material:HeroB\","
            "\"artifactType\":\"Material\","
            "\"loaderId\":\"native-material\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"HeroB.nmat\","
            "\"contentHash\":\"material-b-hash\""
            "}"
            "]"
            "}";
    }

    CountingMeshManager meshManager;
    CountingMaterialManager materialManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerOverride(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerOverride(materialManager);
    ScopedThumbnailResourceManagerAssetPaths paths(root / "Assets", root / "EngineAssets");
    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.LoadPrefabArtifactByAssetId(assetId, "prefab:Hero").has_value());

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.subAssetKey = "prefab:Hero";
    request.artifactPath = "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 64u;
    request.previewRendererVersion = "real-preview:batch-resource-request";
    request.settingsFingerprint = "thumbnail-performance-gpu-preview";
    request.freshnessInputs = {{"artifact", "batch-resource-request:v1"}};

    EditorThumbnailPreviewRenderer renderer(EnsureThumbnailPerformanceTestDriver());
    const auto rendered = renderer.Render(request);

    EXPECT_TRUE(rendered.rgbaPixels.empty());
    EXPECT_EQ(rendered.diagnostic, "thumbnail-gpu-preview-resources-pending");
    EXPECT_EQ(meshManager.asyncRequestCount, 2u);
    EXPECT_TRUE(ContainsPathWithSuffix(meshManager.asyncRequestPaths, "HeroA.nmesh"));
    EXPECT_TRUE(ContainsPathWithSuffix(meshManager.asyncRequestPaths, "HeroB.nmesh"));
    EXPECT_EQ(materialManager.asyncRequestCount, 2u);
    EXPECT_TRUE(ContainsPathWithSuffix(materialManager.asyncRequestPaths, "HeroA.nmat"));
    EXPECT_TRUE(ContainsPathWithSuffix(materialManager.asyncRequestPaths, "HeroB.nmat"));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailPerformanceTests, GpuPreviewPumpsMultipleResourceCompletionsPerFrame)
{
    EXPECT_GE(NLS::Editor::Assets::GetThumbnailPreviewMeshPumpBudgetForTesting(), 4u);
    EXPECT_GE(NLS::Editor::Assets::GetThumbnailPreviewMaterialPumpBudgetForTesting(), 4u);
    EXPECT_GE(NLS::Editor::Assets::GetThumbnailPreviewTexturePumpBudgetForTesting(), 4u);
}

TEST(AssetThumbnailPerformanceTests, MeshManagerPumpAsyncLoadsForPathsLeavesUnrelatedThumbnailRequestsPending)
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

TEST(AssetThumbnailPerformanceTests, MeshManagerKeepsAsyncArtifactQueuedUntilJobSystemExecutorIsAvailable)
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

TEST(AssetThumbnailPerformanceTests, MeshManagerReadyUnrelatedAsyncArtifactsDoNotBlockPathFilteredPromotion)
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

TEST(AssetThumbnailPerformanceTests, MeshManagerSharedRequestRevivesCanceledInFlightArtifactBeforePump)
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

TEST(AssetThumbnailPerformanceTests, MeshManagerCancelableRequestRevivesCanceledInFlightArtifactBeforePump)
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

TEST(AssetThumbnailPerformanceTests, MaterialManagerPumpAsyncLoadsForPathsLeavesUnrelatedThumbnailRequestsPending)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const auto targetPath = root / "Assets" / "target.nmat";
    const auto unrelatedPath = root / "Assets" / "unrelated.nmat";
    WriteNativeArtifactTextFile(
        targetPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "</root>\n");
    WriteNativeArtifactTextFile(
        unrelatedPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "</root>\n");

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    ShaderManager shaderManager;
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);
    ScopedServiceOverride<ShaderManager> shaderManagerOverride(shaderManager);
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
    shaderManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailPerformanceTests, MaterialManagerSharedRequestRevivesCanceledInFlightArtifactBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const auto materialPath = root / "Assets" / "revive-canceled.nmat";
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "</root>\n");

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    ShaderManager shaderManager;
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);
    ScopedServiceOverride<ShaderManager> shaderManagerOverride(shaderManager);
    MaterialManager materialManager;
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
    shaderManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailPerformanceTests, MaterialManagerCancelableRequestRevivesCanceledInFlightArtifactBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    const ScopedThumbnailPerformanceJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());
    const auto root = MakeThumbnailPerformanceRoot();
    const auto materialPath = root / "Assets" / "revive-cancelable.nmat";
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "</root>\n");

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    ShaderManager shaderManager;
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);
    ScopedServiceOverride<ShaderManager> shaderManagerOverride(shaderManager);
    MaterialManager materialManager;
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
    shaderManager.UnloadResources();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailPerformanceTests, TextureManagerSharedRequestRevivesCanceledInFlightArtifactBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async texture request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    EnsureThumbnailPerformanceTestDriver();
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
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(texturePath.string()));

    TextureManager::ClearAsyncArtifactRequestStateForTesting();
    textureManager.UnloadResources();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailPerformanceTests, TextureManagerCancelableRequestRevivesCanceledInFlightArtifactBeforePump)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async texture request state.";
#else
    using namespace NLS::Core::ResourceManagement;

    EnsureThumbnailPerformanceTestDriver();
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
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(texturePath.string()));
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadFailed(texturePath.string()));

    TextureManager::ClearAsyncArtifactRequestStateForTesting();
    textureManager.UnloadResources();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailPerformanceTests, GpuPreviewReadbackPendingIsRepolledByRendererPump)
{
    const ScopedThumbnailPerformanceJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    const auto request = MakeGpuPreviewRequest(root);
    PendingThenReadyPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(pending->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForGpu);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

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

TEST(AssetThumbnailPerformanceTests, RetryableGpuPreviewFailureIsRequeuedByRendererPump)
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
    EXPECT_EQ(fallback->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(
        fallback->diagnostic,
        "thumbnail-gpu-preview-readback-failed:previous async readback has not been completed");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u)
        << "Retryable GPU preview failures must stay in the queue; otherwise thumbnails remain fallback forever.";

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

TEST(AssetThumbnailPerformanceTests, GpuPreviewInFlightCacheWriteIsCancelledDuringServiceShutdown)
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

TEST(AssetThumbnailPerformanceTests, PreviewLoadPolicyAvoidsRuntimeLifecycleAndSynchronousResourceWaits)
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

TEST(AssetThumbnailPerformanceTests, PreviewRenderableSnapshotUsesPrefabGraphDependenciesWithoutInstantiatingPrefab)
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

TEST(AssetThumbnailPerformanceTests, PreviewRenderableSnapshotIncludesPathOnlyRendererDependencies)
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

TEST(AssetThumbnailPerformanceTests, PreviewRenderableSnapshotFlattensParentTransformsForChildRenderers)
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

TEST(AssetThumbnailPerformanceTests, PreviewRenderableSnapshotReadsImportedTransformComponents)
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

TEST(AssetThumbnailPerformanceTests, PreviewRenderableSnapshotFlattensImportedTransformComponentHierarchy)
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
