#include "Engine/Assets/PrefabAsset.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Core/ServiceLocator.h"
#include "Engine/SceneSystem/Scene.h"
#include "GameObject.h"
#include "Guid.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
using NLS::Base::Profiling::PerformanceStageDomain;
using NLS::Base::Profiling::PerformanceStageEntry;
using NLS::Base::Profiling::PerformanceStageStats;
using NLS::Base::Profiling::PerformanceStageStatsCapture;

template<typename T>
class ScopedServiceOverride
{
public:
    explicit ScopedServiceOverride(T& service)
        : m_hadPrevious(NLS::Core::ServiceLocator::Contains<T>())
        , m_previous(m_hadPrevious ? &NLS::Core::ServiceLocator::Get<T>() : nullptr)
    {
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

std::chrono::microseconds Median(std::vector<std::chrono::microseconds> samples)
{
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2u];
}

std::filesystem::path RequiredArtifactPath(const char* environmentVariable)
{
    const auto* value = std::getenv(environmentVariable);
    if (value == nullptr || std::string(value).empty())
        return {};
    return std::filesystem::path(value);
}

std::chrono::microseconds ElapsedSince(
    const std::chrono::steady_clock::time_point begin)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin);
}

std::pair<std::vector<uint8_t>, std::chrono::microseconds> ReadMeshArtifactUsingCurrentPath(
    const std::filesystem::path& path)
{
    const auto begin = std::chrono::steady_clock::now();
    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> bytes;
    std::array<char, 64u * 1024u> buffer {};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto readCount = input.gcount();
        if (readCount <= 0)
            break;

        const auto* chunkBegin = reinterpret_cast<const uint8_t*>(buffer.data());
        bytes.insert(bytes.end(), chunkBegin, chunkBegin + static_cast<size_t>(readCount));
    }
    return {std::move(bytes), ElapsedSince(begin)};
}

std::pair<std::vector<uint8_t>, std::chrono::microseconds> ReadArtifactUsingSingleIfstream(
    const std::filesystem::path& path)
{
    const auto begin = std::chrono::steady_clock::now();
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};

    const auto endPosition = input.tellg();
    if (endPosition <= 0 ||
        static_cast<uintmax_t>(endPosition) >
            static_cast<uintmax_t>((std::numeric_limits<std::streamsize>::max)()))
    {
        return {};
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(endPosition));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
        return {};
    return {std::move(bytes), ElapsedSince(begin)};
}

#if defined(_WIN32)
std::pair<std::vector<uint8_t>, std::chrono::microseconds> ReadArtifactUsingSequentialReadFile(
    const std::filesystem::path& path)
{
    const auto begin = std::chrono::steady_clock::now();
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    LARGE_INTEGER fileSize {};
    if (!GetFileSizeEx(file, &fileSize) ||
        fileSize.QuadPart <= 0 ||
        static_cast<uint64_t>(fileSize.QuadPart) >
            static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
    {
        CloseHandle(file);
        return {};
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize.QuadPart));
    size_t offset = 0u;
    while (offset < bytes.size())
    {
        const auto chunkSize = static_cast<DWORD>(std::min<size_t>(
            bytes.size() - offset,
            8u * 1024u * 1024u));
        DWORD bytesRead = 0u;
        if (!ReadFile(file, bytes.data() + offset, chunkSize, &bytesRead, nullptr) ||
            bytesRead != chunkSize)
        {
            CloseHandle(file);
            return {};
        }
        offset += bytesRead;
    }
    CloseHandle(file);
    return {std::move(bytes), ElapsedSince(begin)};
}
#endif

std::chrono::microseconds FindArtifactTelemetryElapsed(
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage)
{
    const auto records = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    std::chrono::microseconds elapsed {0};
    for (const auto& record : records)
    {
        if (record.stage == stage)
            elapsed += record.elapsed;
    }
    return elapsed;
}

void WriteComparisonReport(const std::string& reportName, const std::string& contents)
{
    const auto* reportDirectory = std::getenv("NLS_PERFORMANCE_REPORT_DIR");
    if (reportDirectory == nullptr || std::string(reportDirectory).empty())
        return;

    std::filesystem::create_directories(reportDirectory);
    std::ofstream output(
        std::filesystem::path(reportDirectory) / (reportName + ".txt"),
        std::ios::binary | std::ios::trunc);
    output << contents;
}

std::chrono::microseconds MeasureSceneRegistration(const size_t objectCount, const bool batch)
{
    NLS::Engine::SceneSystem::Scene scene;
    std::vector<NLS::Engine::GameObject*> gameObjects;
    gameObjects.reserve(objectCount);
    for (size_t index = 0u; index < objectCount; ++index)
    {
        gameObjects.push_back(new NLS::Engine::GameObject(
            "SceneObject_" + std::to_string(index)));
    }

    const auto begin = std::chrono::steady_clock::now();
    if (batch)
    {
        EXPECT_TRUE(scene.AddGameObjects(gameObjects));
    }
    else
    {
        for (auto* gameObject : gameObjects)
            EXPECT_TRUE(scene.AddGameObject(gameObject));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin);
    EXPECT_EQ(scene.GetGameObjects().size(), objectCount);
    return elapsed;
}

uint64_t MeasureSceneRegistrationRebuildCount(const size_t objectCount, const bool batch)
{
    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        (void)MeasureSceneRegistration(objectCount, batch);
    }

    const auto snapshot = stats.Snapshot();
    const auto found = std::find_if(
        snapshot.stages.begin(),
        snapshot.stages.end(),
        [](const PerformanceStageEntry& entry)
        {
            return entry.domain == PerformanceStageDomain::Prefab &&
                entry.stageName == "SceneRebuildFastAccess";
        });
    EXPECT_NE(found, snapshot.stages.end());
    return found != snapshot.stages.end()
        ? found->counters.at("rebuildCount")
        : 0u;
}

NLS::Engine::Assets::PrefabArtifact MakeOpenBenchmarkPrefab(const size_t objectCount)
{
    NLS::Engine::GameObject root("OpenBenchmarkRoot", "Prefab");
    std::vector<std::unique_ptr<NLS::Engine::GameObject>> children;
    children.reserve(objectCount > 0u ? objectCount - 1u : 0u);
    for (size_t index = 1u; index < objectCount; ++index)
    {
        auto child = std::make_unique<NLS::Engine::GameObject>(
            "OpenBenchmarkChild_" + std::to_string(index),
            "Prefab");
        child->SetParent(root);
        children.push_back(std::move(child));
    }

    const auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);
    for (auto iterator = children.rbegin(); iterator != children.rend(); ++iterator)
        (*iterator)->DetachFromParent();

    auto imported = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(document.graph),
        NLS::Core::Assets::AssetId(
            NLS::Guid::Parse("30303030-3030-4030-8030-303030303030")));
    EXPECT_FALSE(imported.diagnostics.HasErrors());
    return std::move(imported.artifact);
}

std::chrono::microseconds MeasurePrefabOpen(
    NLS::Engine::Assets::PrefabArtifact& artifact,
    const bool copyArtifactBeforeInstantiation)
{
    const auto begin = std::chrono::steady_clock::now();
    auto stageScene = std::make_unique<NLS::Engine::SceneSystem::Scene>();
    NLS::Engine::Assets::PrefabArtifactInstantiationResult instantiated;
    if (copyArtifactBeforeInstantiation)
    {
        auto stageArtifact = artifact;
        instantiated = NLS::Engine::Assets::InstantiatePrefabArtifact(stageArtifact, *stageScene);
    }
    else
    {
        instantiated = NLS::Engine::Assets::InstantiatePrefabArtifact(
            std::as_const(artifact),
            *stageScene);
    }
    auto openedGraph = artifact.graph;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin);

    EXPECT_FALSE(instantiated.diagnostics.HasErrors());
    EXPECT_NE(instantiated.root, nullptr);
    EXPECT_EQ(openedGraph.objects.size(), artifact.graph.objects.size());
    return elapsed;
}
}

TEST(ScenePrefabOpenPerformanceTests, SceneBatchRegistrationComparison)
{
    constexpr size_t objectCount = 1000u;
    constexpr size_t sampleCount = 7u;

    (void)MeasureSceneRegistration(objectCount, false);
    (void)MeasureSceneRegistration(objectCount, true);

    std::vector<std::chrono::microseconds> sequentialSamples;
    std::vector<std::chrono::microseconds> batchSamples;
    sequentialSamples.reserve(sampleCount);
    batchSamples.reserve(sampleCount);
    for (size_t sample = 0u; sample < sampleCount; ++sample)
    {
        sequentialSamples.push_back(MeasureSceneRegistration(objectCount, false));
        batchSamples.push_back(MeasureSceneRegistration(objectCount, true));
    }

    const auto sequentialMedian = Median(std::move(sequentialSamples));
    const auto batchMedian = Median(std::move(batchSamples));
    const auto sequentialRebuildCount = MeasureSceneRegistrationRebuildCount(64u, false);
    const auto batchRebuildCount = MeasureSceneRegistrationRebuildCount(64u, true);
    ASSERT_GT(sequentialMedian.count(), 0);
    ASSERT_GT(batchMedian.count(), 0);
    EXPECT_EQ(sequentialRebuildCount, 64u);
    EXPECT_EQ(batchRebuildCount, 1u);
    EXPECT_LT(batchMedian, sequentialMedian);

    const auto improvement = 100.0 *
        static_cast<double>(sequentialMedian.count() - batchMedian.count()) /
        static_cast<double>(sequentialMedian.count());
    const auto report =
        "ObjectCount=" + std::to_string(objectCount) + "\n" +
        "SampleCount=" + std::to_string(sampleCount) + "\n" +
        "SequentialMedianUs=" + std::to_string(sequentialMedian.count()) + "\n" +
        "BatchMedianUs=" + std::to_string(batchMedian.count()) + "\n" +
        "ImprovementPercent=" + std::to_string(improvement) + "\n" +
        "SequentialRebuildCount=" + std::to_string(sequentialRebuildCount) + "\n" +
        "BatchRebuildCount=" + std::to_string(batchRebuildCount) + "\n";
    WriteComparisonReport("SceneRegistration_Comparison", report);
    std::cout << report;
}

TEST(ScenePrefabOpenPerformanceTests, PrefabArtifactCopyComparison)
{
    constexpr size_t gameObjectCount = 1000u;
    constexpr size_t sampleCount = 9u;
    auto artifact = MakeOpenBenchmarkPrefab(gameObjectCount);

    (void)MeasurePrefabOpen(artifact, true);
    (void)MeasurePrefabOpen(artifact, false);

    std::vector<std::chrono::microseconds> copiedSamples;
    std::vector<std::chrono::microseconds> constSamples;
    copiedSamples.reserve(sampleCount);
    constSamples.reserve(sampleCount);
    for (size_t sample = 0u; sample < sampleCount; ++sample)
    {
        copiedSamples.push_back(MeasurePrefabOpen(artifact, true));
        constSamples.push_back(MeasurePrefabOpen(artifact, false));
    }

    const auto copiedMedian = Median(std::move(copiedSamples));
    const auto constMedian = Median(std::move(constSamples));
    ASSERT_GT(copiedMedian.count(), 0);
    ASSERT_GT(constMedian.count(), 0);
    EXPECT_LT(constMedian, copiedMedian);

    const auto improvement = 100.0 *
        static_cast<double>(copiedMedian.count() - constMedian.count()) /
        static_cast<double>(copiedMedian.count());
    const auto report =
        "GameObjectCount=" + std::to_string(gameObjectCount) + "\n" +
        "GraphObjectCount=" + std::to_string(artifact.graph.objects.size()) + "\n" +
        "SampleCount=" + std::to_string(sampleCount) + "\n" +
        "CopiedArtifactMedianUs=" + std::to_string(copiedMedian.count()) + "\n" +
        "ConstArtifactMedianUs=" + std::to_string(constMedian.count()) + "\n" +
        "ImprovementPercent=" + std::to_string(improvement) + "\n";
    WriteComparisonReport("PrefabOpen_Comparison", report);
    std::cout << report;
}

TEST(ScenePrefabOpenPerformanceTests, ImportedMeshAndTextureArtifactLoadBreakdown)
{
    constexpr size_t warmSampleCount = 7u;
    const auto meshPath = RequiredArtifactPath("NLS_PERFORMANCE_MESH_ARTIFACT");
    const auto texturePath = RequiredArtifactPath("NLS_PERFORMANCE_TEXTURE_ARTIFACT");
    if (meshPath.empty() || texturePath.empty())
        GTEST_SKIP() << "Set NLS_PERFORMANCE_MESH_ARTIFACT and NLS_PERFORMANCE_TEXTURE_ARTIFACT.";

    ASSERT_TRUE(std::filesystem::is_regular_file(meshPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(texturePath));

    const auto meshFirstBegin = std::chrono::steady_clock::now();
    NLS::Core::Assets::SetArtifactLoadTelemetryEnabled(true);
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto firstMesh = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    const auto meshFirst = ElapsedSince(meshFirstBegin);
    const auto meshFirstRead = FindArtifactTelemetryElapsed(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead);
    const auto meshFirstDeserialize = FindArtifactTelemetryElapsed(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize);
    const auto meshFirstContainerValidation = FindArtifactTelemetryElapsed(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeContainerParseHash);
    ASSERT_TRUE(firstMesh.has_value());

    std::vector<std::chrono::microseconds> meshOptimizedTotalSamples;
    std::vector<std::chrono::microseconds> meshOptimizedReadSamples;
    std::vector<std::chrono::microseconds> meshOptimizedDeserializeSamples;
    std::vector<std::chrono::microseconds> meshOptimizedContainerValidationSamples;
    std::vector<std::chrono::microseconds> meshOptimizedPayloadCopySamples;
    std::vector<std::chrono::microseconds> meshReadSamples;
    std::vector<std::chrono::microseconds> meshDeserializeSamples;
    std::vector<std::chrono::microseconds> meshTotalSamples;
    std::vector<std::chrono::microseconds> meshSingleIfstreamReadSamples;
#if defined(_WIN32)
    std::vector<std::chrono::microseconds> meshSequentialReadFileSamples;
    std::vector<std::chrono::microseconds> meshBufferedProductionTotalSamples;
#endif
    meshOptimizedTotalSamples.reserve(warmSampleCount);
    meshOptimizedReadSamples.reserve(warmSampleCount);
    meshOptimizedDeserializeSamples.reserve(warmSampleCount);
    meshOptimizedContainerValidationSamples.reserve(warmSampleCount);
    meshOptimizedPayloadCopySamples.reserve(warmSampleCount);
    meshReadSamples.reserve(warmSampleCount);
    meshDeserializeSamples.reserve(warmSampleCount);
    meshTotalSamples.reserve(warmSampleCount);
    meshSingleIfstreamReadSamples.reserve(warmSampleCount);
#if defined(_WIN32)
    meshSequentialReadFileSamples.reserve(warmSampleCount);
    meshBufferedProductionTotalSamples.reserve(warmSampleCount);
#endif
    for (size_t sample = 0u; sample < warmSampleCount; ++sample)
    {
        NLS::Core::Assets::ClearArtifactLoadTelemetry();
        const auto optimizedTotalBegin = std::chrono::steady_clock::now();
        const auto optimizedMesh = NLS::Render::Assets::LoadMeshArtifact(meshPath);
        meshOptimizedTotalSamples.push_back(ElapsedSince(optimizedTotalBegin));
        ASSERT_TRUE(optimizedMesh.has_value());
        meshOptimizedReadSamples.push_back(FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead));
        meshOptimizedDeserializeSamples.push_back(FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize));
        meshOptimizedContainerValidationSamples.push_back(FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeContainerParseHash));
        meshOptimizedPayloadCopySamples.push_back(FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy));

        const auto totalBegin = std::chrono::steady_clock::now();
        auto [bytes, readElapsed] = ReadMeshArtifactUsingCurrentPath(meshPath);
        ASSERT_FALSE(bytes.empty());
        const auto deserializeBegin = std::chrono::steady_clock::now();
        const auto mesh = NLS::Render::Assets::DeserializeMeshArtifact(bytes);
        const auto deserializeElapsed = ElapsedSince(deserializeBegin);
        ASSERT_TRUE(mesh.has_value());

        meshReadSamples.push_back(readElapsed);
        meshDeserializeSamples.push_back(deserializeElapsed);
        meshTotalSamples.push_back(ElapsedSince(totalBegin));

        auto [singleIfstreamBytes, singleIfstreamElapsed] =
            ReadArtifactUsingSingleIfstream(meshPath);
        ASSERT_EQ(singleIfstreamBytes.size(), std::filesystem::file_size(meshPath));
        meshSingleIfstreamReadSamples.push_back(singleIfstreamElapsed);
#if defined(_WIN32)
        auto [sequentialReadFileBytes, sequentialReadFileElapsed] =
            ReadArtifactUsingSequentialReadFile(meshPath);
        ASSERT_EQ(sequentialReadFileBytes.size(), std::filesystem::file_size(meshPath));
        meshSequentialReadFileSamples.push_back(sequentialReadFileElapsed);

        const auto bufferedProductionBegin = std::chrono::steady_clock::now();
        const auto bufferedMesh =
            NLS::Render::Assets::DeserializeMeshArtifactTrustedForTesting(
                sequentialReadFileBytes);
        meshBufferedProductionTotalSamples.push_back(
            sequentialReadFileElapsed + ElapsedSince(bufferedProductionBegin));
        ASSERT_TRUE(bufferedMesh.has_value());
#endif
    }

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto textureFirstBegin = std::chrono::steady_clock::now();
    const auto firstTexture = NLS::Render::Assets::LoadTextureArtifact(texturePath);
    const auto textureFirst = ElapsedSince(textureFirstBegin);
    const auto textureFirstRead = FindArtifactTelemetryElapsed(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead);
    const auto textureFirstDeserialize = FindArtifactTelemetryElapsed(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize);
    const auto textureFirstContainerValidation = FindArtifactTelemetryElapsed(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeContainerParseHash);
    ASSERT_TRUE(firstTexture.has_value());

    std::vector<std::chrono::microseconds> textureReadSamples;
    std::vector<std::chrono::microseconds> textureDeserializeSamples;
    std::vector<std::chrono::microseconds> textureTotalSamples;
    std::vector<std::chrono::microseconds> textureContainerValidationSamples;
    std::vector<std::chrono::microseconds> textureSingleIfstreamReadSamples;
#if defined(_WIN32)
    std::vector<std::chrono::microseconds> textureSequentialReadFileSamples;
#endif
    textureReadSamples.reserve(warmSampleCount);
    textureDeserializeSamples.reserve(warmSampleCount);
    textureTotalSamples.reserve(warmSampleCount);
    textureContainerValidationSamples.reserve(warmSampleCount);
    textureSingleIfstreamReadSamples.reserve(warmSampleCount);
#if defined(_WIN32)
    textureSequentialReadFileSamples.reserve(warmSampleCount);
#endif
    for (size_t sample = 0u; sample < warmSampleCount; ++sample)
    {
        NLS::Core::Assets::ClearArtifactLoadTelemetry();
        const auto totalBegin = std::chrono::steady_clock::now();
        const auto texture = NLS::Render::Assets::LoadTextureArtifact(texturePath);
        const auto totalElapsed = ElapsedSince(totalBegin);
        ASSERT_TRUE(texture.has_value());
        textureReadSamples.push_back(FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead));
        textureDeserializeSamples.push_back(FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize));
        textureContainerValidationSamples.push_back(FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeContainerParseHash));
        textureTotalSamples.push_back(totalElapsed);

        auto [singleIfstreamBytes, singleIfstreamElapsed] =
            ReadArtifactUsingSingleIfstream(texturePath);
        ASSERT_EQ(singleIfstreamBytes.size(), std::filesystem::file_size(texturePath));
        textureSingleIfstreamReadSamples.push_back(singleIfstreamElapsed);
#if defined(_WIN32)
        auto [sequentialReadFileBytes, sequentialReadFileElapsed] =
            ReadArtifactUsingSequentialReadFile(texturePath);
        ASSERT_EQ(sequentialReadFileBytes.size(), std::filesystem::file_size(texturePath));
        textureSequentialReadFileSamples.push_back(sequentialReadFileElapsed);
#endif
    }

    const auto meshOptimizedTotalMedian = Median(std::move(meshOptimizedTotalSamples));
    const auto meshOptimizedReadMedian = Median(std::move(meshOptimizedReadSamples));
    const auto meshOptimizedDeserializeMedian = Median(std::move(meshOptimizedDeserializeSamples));
    const auto meshOptimizedContainerValidationMedian = Median(
        std::move(meshOptimizedContainerValidationSamples));
    const auto meshOptimizedPayloadCopyMedian = Median(
        std::move(meshOptimizedPayloadCopySamples));
    const auto meshReadMedian = Median(std::move(meshReadSamples));
    const auto meshDeserializeMedian = Median(std::move(meshDeserializeSamples));
    const auto meshTotalMedian = Median(std::move(meshTotalSamples));
    const auto meshSingleIfstreamReadMedian = Median(std::move(meshSingleIfstreamReadSamples));
#if defined(_WIN32)
    const auto meshSequentialReadFileMedian = Median(std::move(meshSequentialReadFileSamples));
    const auto meshBufferedProductionTotalMedian = Median(
        std::move(meshBufferedProductionTotalSamples));
    const auto meshDirectLoadImprovementPercent = 100.0 *
        static_cast<double>(
            meshBufferedProductionTotalMedian.count() - meshOptimizedTotalMedian.count()) /
        static_cast<double>(meshBufferedProductionTotalMedian.count());
#endif
    const auto textureReadMedian = Median(std::move(textureReadSamples));
    const auto textureDeserializeMedian = Median(std::move(textureDeserializeSamples));
    const auto textureTotalMedian = Median(std::move(textureTotalSamples));
    const auto textureContainerValidationMedian = Median(std::move(textureContainerValidationSamples));
    const auto textureSingleIfstreamReadMedian = Median(std::move(textureSingleIfstreamReadSamples));
#if defined(_WIN32)
    const auto textureSequentialReadFileMedian = Median(std::move(textureSequentialReadFileSamples));
#endif
    const auto meshImprovementPercent = 100.0 *
        static_cast<double>(meshTotalMedian.count() - meshOptimizedTotalMedian.count()) /
        static_cast<double>(meshTotalMedian.count());

    const auto report =
        "WarmSampleCount=" + std::to_string(warmSampleCount) + "\n" +
        "MeshPath=" + meshPath.generic_string() + "\n" +
        "MeshBytes=" + std::to_string(std::filesystem::file_size(meshPath)) + "\n" +
        "MeshOptimizedFirstUs=" + std::to_string(meshFirst.count()) + "\n" +
        "MeshOptimizedFirstReadUs=" + std::to_string(meshFirstRead.count()) + "\n" +
        "MeshOptimizedFirstDeserializeUs=" + std::to_string(meshFirstDeserialize.count()) + "\n" +
        "MeshOptimizedFirstContainerValidationUs=" + std::to_string(meshFirstContainerValidation.count()) + "\n" +
        "MeshLegacyWarmReadMedianUs=" + std::to_string(meshReadMedian.count()) + "\n" +
        "MeshLegacyWarmDeserializeMedianUs=" + std::to_string(meshDeserializeMedian.count()) + "\n" +
        "MeshLegacyWarmTotalMedianUs=" + std::to_string(meshTotalMedian.count()) + "\n" +
        "MeshOptimizedWarmTotalMedianUs=" + std::to_string(meshOptimizedTotalMedian.count()) + "\n" +
        "MeshOptimizedWarmReadMedianUs=" + std::to_string(meshOptimizedReadMedian.count()) + "\n" +
        "MeshOptimizedWarmDeserializeMedianUs=" + std::to_string(meshOptimizedDeserializeMedian.count()) + "\n" +
        "MeshOptimizedWarmContainerValidationMedianUs=" + std::to_string(meshOptimizedContainerValidationMedian.count()) + "\n" +
        "MeshOptimizedWarmPayloadCopyMedianUs=" + std::to_string(meshOptimizedPayloadCopyMedian.count()) + "\n" +
        "MeshSingleIfstreamReadMedianUs=" + std::to_string(meshSingleIfstreamReadMedian.count()) + "\n" +
#if defined(_WIN32)
        "MeshSequentialReadFileMedianUs=" + std::to_string(meshSequentialReadFileMedian.count()) + "\n" +
        "MeshBufferedProductionWarmTotalMedianUs=" + std::to_string(meshBufferedProductionTotalMedian.count()) + "\n" +
        "MeshDirectLoadImprovementPercent=" + std::to_string(meshDirectLoadImprovementPercent) + "\n" +
#endif
        "MeshImprovementPercent=" + std::to_string(meshImprovementPercent) + "\n" +
        "TexturePath=" + texturePath.generic_string() + "\n" +
        "TextureBytes=" + std::to_string(std::filesystem::file_size(texturePath)) + "\n" +
        "TextureFirstUs=" + std::to_string(textureFirst.count()) + "\n" +
        "TextureFirstReadUs=" + std::to_string(textureFirstRead.count()) + "\n" +
        "TextureFirstDeserializeUs=" + std::to_string(textureFirstDeserialize.count()) + "\n" +
        "TextureFirstContainerValidationUs=" + std::to_string(textureFirstContainerValidation.count()) + "\n" +
        "TextureWarmReadMedianUs=" + std::to_string(textureReadMedian.count()) + "\n" +
        "TextureWarmDeserializeMedianUs=" + std::to_string(textureDeserializeMedian.count()) + "\n" +
        "TextureWarmContainerValidationMedianUs=" + std::to_string(textureContainerValidationMedian.count()) + "\n" +
        "TextureWarmTotalMedianUs=" + std::to_string(textureTotalMedian.count()) + "\n";
#if defined(_WIN32)
    const auto platformReadReport =
        "TextureSingleIfstreamReadMedianUs=" + std::to_string(textureSingleIfstreamReadMedian.count()) + "\n" +
        "TextureSequentialReadFileMedianUs=" + std::to_string(textureSequentialReadFileMedian.count()) + "\n";
#else
    const auto platformReadReport =
        "TextureSingleIfstreamReadMedianUs=" + std::to_string(textureSingleIfstreamReadMedian.count()) + "\n";
#endif
    WriteComparisonReport("ImportedArtifactLoad_Comparison", report + platformReadReport);
    std::cout << report << platformReadReport;
}

TEST(ScenePrefabOpenPerformanceTests, ContentAddressedDirectMeshLoadMatchesBufferedPayload)
{
    const auto meshPath = RequiredArtifactPath("NLS_PERFORMANCE_MESH_ARTIFACT");
    if (meshPath.empty())
        GTEST_SKIP() << "Set NLS_PERFORMANCE_MESH_ARTIFACT.";

    const auto direct = NLS::Render::Assets::LoadMeshArtifact(meshPath);
#if defined(_WIN32)
    auto [bytes, readElapsed] = ReadArtifactUsingSequentialReadFile(meshPath);
#else
    auto [bytes, readElapsed] = ReadArtifactUsingSingleIfstream(meshPath);
#endif
    (void)readElapsed;
    const auto buffered =
        NLS::Render::Assets::DeserializeMeshArtifactTrustedForTesting(bytes);

    ASSERT_TRUE(direct.has_value());
    ASSERT_TRUE(buffered.has_value());
    ASSERT_EQ(direct->vertices.size(), buffered->vertices.size());
    ASSERT_EQ(direct->indices.size(), buffered->indices.size());
    EXPECT_EQ(direct->materialIndex, buffered->materialIndex);
    EXPECT_EQ(direct->hasBoundingSphere, buffered->hasBoundingSphere);
    EXPECT_FLOAT_EQ(direct->boundingSphere.position.x, buffered->boundingSphere.position.x);
    EXPECT_FLOAT_EQ(direct->boundingSphere.position.y, buffered->boundingSphere.position.y);
    EXPECT_FLOAT_EQ(direct->boundingSphere.position.z, buffered->boundingSphere.position.z);
    EXPECT_FLOAT_EQ(direct->boundingSphere.radius, buffered->boundingSphere.radius);
    if (!direct->vertices.empty())
    {
        EXPECT_EQ(
            std::memcmp(
                direct->vertices.data(),
                buffered->vertices.data(),
                direct->vertices.size() * sizeof(NLS::Render::Geometry::Vertex)),
            0);
    }
    if (!direct->indices.empty())
    {
        EXPECT_EQ(
            std::memcmp(
                direct->indices.data(),
                buffered->indices.data(),
                direct->indices.size() * sizeof(uint32_t)),
            0);
    }
}

TEST(ScenePrefabOpenPerformanceTests, ImportedTextureDx12UploadBreakdown)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 upload measurement is Windows-only.";
#else
    constexpr size_t warmSampleCount = 7u;
    const auto texturePath = RequiredArtifactPath("NLS_PERFORMANCE_TEXTURE_ARTIFACT");
    if (texturePath.empty())
        GTEST_SKIP() << "Set NLS_PERFORMANCE_TEXTURE_ARTIFACT.";

    ASSERT_TRUE(std::filesystem::is_regular_file(texturePath));
    NLS::Render::Settings::DriverSettings driverSettings;
    driverSettings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    driverSettings.debugMode = false;
    driverSettings.enableThreadedRendering = false;
    NLS::Render::Context::Driver driver(driverSettings);
    ASSERT_EQ(
        driver.GetActiveGraphicsBackend(),
        NLS::Render::Settings::EGraphicsBackend::DX12);
    ScopedServiceOverride<NLS::Render::Context::Driver> driverOverride(driver);

    const auto bufferedReference =
        NLS::Render::Assets::LoadTextureArtifactBufferedForTesting(texturePath);
    const auto mappedReference = NLS::Render::Assets::LoadTextureArtifact(texturePath);
    ASSERT_TRUE(bufferedReference.has_value());
    ASSERT_TRUE(mappedReference.has_value());
    ASSERT_EQ(bufferedReference->width, mappedReference->width);
    ASSERT_EQ(bufferedReference->height, mappedReference->height);
    ASSERT_EQ(bufferedReference->format, mappedReference->format);
    ASSERT_EQ(bufferedReference->mips.size(), mappedReference->mips.size());
    for (size_t mipIndex = 0u; mipIndex < bufferedReference->mips.size(); ++mipIndex)
    {
        const auto& bufferedMip = bufferedReference->mips[mipIndex];
        const auto& mappedMip = mappedReference->mips[mipIndex];
        ASSERT_EQ(bufferedMip.PixelSize(), mappedMip.PixelSize());
        EXPECT_EQ(
            std::memcmp(
                bufferedMip.PixelData(),
                mappedMip.PixelData(),
                bufferedMip.PixelSize()),
            0);
    }

    struct LoadAndUploadMeasurement
    {
        std::chrono::microseconds total {};
        std::chrono::microseconds artifactLoad {};
        std::chrono::microseconds fileRead {};
        std::chrono::microseconds fileMap {};
        std::chrono::microseconds deserialize {};
        std::chrono::microseconds gpuUpload {};
        std::chrono::microseconds resourceCreate {};
        std::chrono::microseconds prepare {};
        std::chrono::microseconds cpuCopy {};
        std::chrono::microseconds commandSetup {};
        std::chrono::microseconds submit {};
        std::chrono::microseconds fenceWait {};
    };

    const auto measure = [&](const bool useMapping)
    {
        NLS::Core::Assets::ClearArtifactLoadTelemetry();
        const auto totalBegin = std::chrono::steady_clock::now();
        const auto artifactBegin = std::chrono::steady_clock::now();
        auto artifact = useMapping
            ? NLS::Render::Assets::LoadTextureArtifact(texturePath)
            : NLS::Render::Assets::LoadTextureArtifactBufferedForTesting(texturePath);
        LoadAndUploadMeasurement measurement;
        measurement.artifactLoad = ElapsedSince(artifactBegin);
        EXPECT_TRUE(artifact.has_value());
        if (!artifact.has_value())
            return measurement;

        auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreateFromArtifact(
            *artifact,
            NLS::Render::Settings::ETextureFilteringMode::LINEAR,
            NLS::Render::Settings::ETextureFilteringMode::LINEAR,
            false);
        measurement.total = ElapsedSince(totalBegin);
        EXPECT_NE(texture, nullptr);
        if (texture != nullptr)
        {
            EXPECT_NE(texture->GetTextureHandle(), nullptr);
            NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture);
        }
        measurement.fileRead = FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead);
        measurement.fileMap = FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileMap);
        measurement.deserialize = FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize);
        measurement.gpuUpload = FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::GpuUpload);
        measurement.resourceCreate = FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::GpuResourceCreate);
        measurement.prepare = FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::GpuUploadPrepare);
        measurement.cpuCopy = FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::GpuUploadCpuCopy);
        measurement.commandSetup = FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::GpuUploadCommandSetup);
        measurement.submit = FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::GpuUploadSubmit);
        measurement.fenceWait = FindArtifactTelemetryElapsed(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::GpuUploadFenceWait);
        return measurement;
    };

    std::vector<std::chrono::microseconds> baselineTotalSamples;
    std::vector<std::chrono::microseconds> mappedTotalSamples;
    std::vector<std::chrono::microseconds> baselineArtifactLoadSamples;
    std::vector<std::chrono::microseconds> mappedArtifactLoadSamples;
    std::vector<std::chrono::microseconds> baselineFileReadSamples;
    std::vector<std::chrono::microseconds> mappedFileMapSamples;
    std::vector<std::chrono::microseconds> baselineGpuUploadSamples;
    std::vector<std::chrono::microseconds> mappedGpuUploadSamples;
    std::vector<std::chrono::microseconds> mappedCpuCopySamples;
    std::vector<std::chrono::microseconds> mappedResourceCreateSamples;
    std::vector<std::chrono::microseconds> mappedPrepareSamples;
    std::vector<std::chrono::microseconds> mappedCommandSetupSamples;
    std::vector<std::chrono::microseconds> mappedSubmitSamples;
    std::vector<std::chrono::microseconds> mappedFenceWaitSamples;
    baselineTotalSamples.reserve(warmSampleCount);
    mappedTotalSamples.reserve(warmSampleCount);

    NLS::Core::Assets::SetArtifactLoadTelemetryEnabled(true);
    (void)measure(false);
    (void)measure(true);
    for (size_t sample = 0u; sample < warmSampleCount; ++sample)
    {
        LoadAndUploadMeasurement baseline;
        LoadAndUploadMeasurement mapped;
        if ((sample & 1u) == 0u)
        {
            baseline = measure(false);
            mapped = measure(true);
        }
        else
        {
            mapped = measure(true);
            baseline = measure(false);
        }

        baselineTotalSamples.push_back(baseline.total);
        mappedTotalSamples.push_back(mapped.total);
        baselineArtifactLoadSamples.push_back(baseline.artifactLoad);
        mappedArtifactLoadSamples.push_back(mapped.artifactLoad);
        baselineFileReadSamples.push_back(baseline.fileRead);
        mappedFileMapSamples.push_back(mapped.fileMap);
        baselineGpuUploadSamples.push_back(baseline.gpuUpload);
        mappedGpuUploadSamples.push_back(mapped.gpuUpload);
        mappedCpuCopySamples.push_back(mapped.cpuCopy);
        mappedResourceCreateSamples.push_back(mapped.resourceCreate);
        mappedPrepareSamples.push_back(mapped.prepare);
        mappedCommandSetupSamples.push_back(mapped.commandSetup);
        mappedSubmitSamples.push_back(mapped.submit);
        mappedFenceWaitSamples.push_back(mapped.fenceWait);
    }

    const auto baselineTotalMedian = Median(std::move(baselineTotalSamples));
    const auto mappedTotalMedian = Median(std::move(mappedTotalSamples));
    const auto baselineArtifactLoadMedian = Median(std::move(baselineArtifactLoadSamples));
    const auto mappedArtifactLoadMedian = Median(std::move(mappedArtifactLoadSamples));
    const auto baselineFileReadMedian = Median(std::move(baselineFileReadSamples));
    const auto mappedFileMapMedian = Median(std::move(mappedFileMapSamples));
    const auto baselineGpuUploadMedian = Median(std::move(baselineGpuUploadSamples));
    const auto mappedGpuUploadMedian = Median(std::move(mappedGpuUploadSamples));
    const auto mappedCpuCopyMedian = Median(std::move(mappedCpuCopySamples));
    const auto mappedResourceCreateMedian = Median(std::move(mappedResourceCreateSamples));
    const auto mappedPrepareMedian = Median(std::move(mappedPrepareSamples));
    const auto mappedCommandSetupMedian = Median(std::move(mappedCommandSetupSamples));
    const auto mappedSubmitMedian = Median(std::move(mappedSubmitSamples));
    const auto mappedFenceWaitMedian = Median(std::move(mappedFenceWaitSamples));
    const auto totalImprovementPercent = 100.0 *
        static_cast<double>(baselineTotalMedian.count() - mappedTotalMedian.count()) /
        static_cast<double>(baselineTotalMedian.count());
    const auto report =
        "Backend=DX12\n"
        "Configuration=Release\n"
        "WarmSampleCount=" + std::to_string(warmSampleCount) + "\n" +
        "TexturePath=" + texturePath.generic_string() + "\n" +
        "TextureArtifactBytes=" + std::to_string(std::filesystem::file_size(texturePath)) + "\n" +
        "TextureWidth=" + std::to_string(mappedReference->width) + "\n" +
        "TextureHeight=" + std::to_string(mappedReference->height) + "\n" +
        "TextureFormat=" + std::to_string(static_cast<uint32_t>(mappedReference->format)) + "\n" +
        "TextureMipCount=" + std::to_string(mappedReference->mips.size()) + "\n" +
        "BufferedEndToEndMedianUs=" + std::to_string(baselineTotalMedian.count()) + "\n" +
        "MappedEndToEndMedianUs=" + std::to_string(mappedTotalMedian.count()) + "\n" +
        "EndToEndImprovementPercent=" + std::to_string(totalImprovementPercent) + "\n" +
        "BufferedArtifactLoadMedianUs=" + std::to_string(baselineArtifactLoadMedian.count()) + "\n" +
        "MappedArtifactLoadMedianUs=" + std::to_string(mappedArtifactLoadMedian.count()) + "\n" +
        "BufferedFileReadMedianUs=" + std::to_string(baselineFileReadMedian.count()) + "\n" +
        "MappedFileMapMedianUs=" + std::to_string(mappedFileMapMedian.count()) + "\n" +
        "BufferedGpuUploadMedianUs=" + std::to_string(baselineGpuUploadMedian.count()) + "\n" +
        "MappedGpuUploadMedianUs=" + std::to_string(mappedGpuUploadMedian.count()) + "\n" +
        "MappedGpuResourceCreateMedianUs=" + std::to_string(mappedResourceCreateMedian.count()) + "\n" +
        "MappedGpuUploadPrepareMedianUs=" + std::to_string(mappedPrepareMedian.count()) + "\n" +
        "MappedGpuUploadCpuCopyMedianUs=" + std::to_string(mappedCpuCopyMedian.count()) + "\n" +
        "MappedGpuUploadCommandSetupMedianUs=" + std::to_string(mappedCommandSetupMedian.count()) + "\n" +
        "MappedGpuUploadSubmitMedianUs=" + std::to_string(mappedSubmitMedian.count()) + "\n" +
        "MappedGpuUploadFenceWaitMedianUs=" + std::to_string(mappedFenceWaitMedian.count()) + "\n";
    WriteComparisonReport("ImportedTextureMappedLoad_Comparison", report);
    std::cout << report;
#endif
}
