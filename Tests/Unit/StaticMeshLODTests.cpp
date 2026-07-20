#include <gtest/gtest.h>

#include "Rendering/Assets/StaticMeshLODSettings.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/MeshReduction.h"
#include "Rendering/Assets/StaticMeshBuilder.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
using NLS::Render::Assets::BuildStaticMeshLODTargetRatios;
using NLS::Render::Assets::StaticMeshLODSourceKind;
using NLS::Render::Assets::StaticMeshLODSettingsRegistry;
using NLS::Render::Assets::StaticMeshSourceAsset;
using NLS::Render::Assets::StaticMeshSourceModel;
using NLS::Render::Assets::ValidateStaticMeshSourceAsset;
using NLS::Render::Assets::DeserializeMeshArtifactBundle;
using NLS::Render::Assets::MeshArtifactBundle;
using NLS::Render::Assets::MeshArtifactLODResource;
using NLS::Render::Assets::SerializeMeshArtifactBundle;
using NLS::Render::Assets::ReduceMeshArtifact;
using NLS::Render::Assets::BuildStaticMeshLODArtifact;
using NLS::Render::Assets::LoadMeshArtifact;
using NLS::Render::Assets::LoadMeshArtifactBundle;
using NLS::Render::Assets::SelectMeshArtifactLOD;

TEST(StaticMeshLODTests, BuiltinPresetsMatchUE426Defaults)
{
    const StaticMeshLODSettingsRegistry registry;

    const auto* none = registry.Find("None");
    ASSERT_NE(none, nullptr);
    EXPECT_EQ(none->numLODs, 1u);

    const auto* architecture = registry.Find("LevelArchitecture");
    ASSERT_NE(architecture, nullptr);
    EXPECT_EQ(architecture->numLODs, 4u);
    EXPECT_FLOAT_EQ(architecture->lodPercentTriangles, 50.0f);
    EXPECT_FLOAT_EQ(architecture->pixelError, 12.0f);

    const auto* smallProp = registry.Find("SmallProp");
    ASSERT_NE(smallProp, nullptr);
    EXPECT_EQ(smallProp->numLODs, 4u);
    EXPECT_FLOAT_EQ(smallProp->lodPercentTriangles, 50.0f);
    EXPECT_FLOAT_EQ(smallProp->pixelError, 10.0f);

    const auto* highDetail = registry.Find("HighDetail");
    ASSERT_NE(highDetail, nullptr);
    EXPECT_EQ(highDetail->numLODs, 6u);
    EXPECT_FLOAT_EQ(highDetail->lodPercentTriangles, 50.0f);
    EXPECT_FLOAT_EQ(highDetail->pixelError, 6.0f);
}

TEST(StaticMeshLODTests, PresetTargetsAreCumulativeFromLOD0)
{
    const StaticMeshLODSettingsRegistry registry;
    const auto* preset = registry.Find("SmallProp");
    ASSERT_NE(preset, nullptr);

    const auto ratios = BuildStaticMeshLODTargetRatios(*preset);

    ASSERT_EQ(ratios.size(), 4u);
    EXPECT_FLOAT_EQ(ratios[0], 1.0f);
    EXPECT_FLOAT_EQ(ratios[1], 0.5f);
    EXPECT_FLOAT_EQ(ratios[2], 0.25f);
    EXPECT_FLOAT_EQ(ratios[3], 0.125f);
}

TEST(StaticMeshLODTests, SourceAssetRequiresLOD0)
{
    const StaticMeshSourceAsset asset;

    const auto validation = ValidateStaticMeshSourceAsset(asset);

    EXPECT_FALSE(validation.valid);
    ASSERT_EQ(validation.diagnostics.size(), 1u);
    EXPECT_EQ(validation.diagnostics[0], "static-mesh-lod0-missing");
}

TEST(StaticMeshLODTests, SourceAssetRequiresStrictlyDescendingScreenSizes)
{
    StaticMeshSourceAsset asset;
    asset.sourceModels = {
        StaticMeshSourceModel {StaticMeshLODSourceKind::Imported, 1.0f},
        StaticMeshSourceModel {StaticMeshLODSourceKind::Generated, 1.0f}};

    const auto validation = ValidateStaticMeshSourceAsset(asset);

    EXPECT_FALSE(validation.valid);
    ASSERT_EQ(validation.diagnostics.size(), 1u);
    EXPECT_EQ(validation.diagnostics[0], "static-mesh-lod-screen-size-not-descending");
}

TEST(StaticMeshLODTests, ValidSourceAssetPreservesLevelProvenance)
{
    StaticMeshSourceAsset asset;
    asset.sourceModels = {
        StaticMeshSourceModel {StaticMeshLODSourceKind::Imported, 1.0f},
        StaticMeshSourceModel {StaticMeshLODSourceKind::Authored, 0.5f},
        StaticMeshSourceModel {StaticMeshLODSourceKind::Generated, 0.25f}};

    const auto validation = ValidateStaticMeshSourceAsset(asset);

    EXPECT_TRUE(validation.valid);
    EXPECT_TRUE(validation.diagnostics.empty());
    EXPECT_EQ(asset.sourceModels[1].sourceKind, StaticMeshLODSourceKind::Authored);
    EXPECT_EQ(asset.sourceModels[2].sourceKind, StaticMeshLODSourceKind::Generated);
}

TEST(StaticMeshLODTests, MultiLODArtifactRoundTripsAllLevels)
{
    MeshArtifactBundle bundle;
    bundle.lodResources = {
        MeshArtifactLODResource {{}, 1.0f},
        MeshArtifactLODResource {{}, 0.5f}};
    bundle.lodResources[0].mesh.materialIndex = 2u;
    bundle.lodResources[1].mesh.materialIndex = 7u;

    const auto bytes = SerializeMeshArtifactBundle(bundle);
    const auto decoded = DeserializeMeshArtifactBundle(bytes);

    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->lodResources.size(), 2u);
    EXPECT_FLOAT_EQ(decoded->lodResources[0].screenSize, 1.0f);
    EXPECT_FLOAT_EQ(decoded->lodResources[1].screenSize, 0.5f);
    EXPECT_EQ(decoded->lodResources[0].mesh.materialIndex, 2u);
    EXPECT_EQ(decoded->lodResources[1].mesh.materialIndex, 7u);
}

TEST(StaticMeshLODTests, MeshReductionPreservesMaterialAndValidIndices)
{
    const std::vector<NLS::Render::Geometry::Vertex> vertices {
        {{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{1.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{1.0f, 1.0f, 0.0f}, {}, {}, {}, {}},
        {{0.0f, 1.0f, 0.0f}, {}, {}, {}, {}}
    };
    const NLS::Render::Assets::MeshArtifactData source {
        vertices,
        {0u, 1u, 2u, 0u, 2u, 3u},
        9u};

    const auto reduced = ReduceMeshArtifact(source, 1u);

    ASSERT_TRUE(reduced.has_value());
    EXPECT_LE(reduced->indices.size(), 3u);
    EXPECT_EQ(reduced->materialIndex, 9u);
    for (const auto index : reduced->indices)
        EXPECT_LT(index, reduced->vertices.size());
}

TEST(StaticMeshLODTests, MeshReductionRejectsEmptyAndInvalidInput)
{
    EXPECT_FALSE(ReduceMeshArtifact({}, 1u).has_value());

    NLS::Render::Assets::MeshArtifactData invalid;
    invalid.vertices.resize(3u);
    invalid.indices = {0u, 1u, 3u};
    EXPECT_FALSE(ReduceMeshArtifact(invalid, 1u).has_value());
}

TEST(StaticMeshLODTests, MeshReductionKeepsFullTopologyWhenTargetIsAtLeastSource)
{
    const std::vector<NLS::Render::Geometry::Vertex> vertices {
        {{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{1.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{1.0f, 1.0f, 0.0f}, {}, {}, {}, {}}
    };
    const NLS::Render::Assets::MeshArtifactData source {
        vertices,
        {0u, 1u, 2u},
        4u};

    const auto reduced = ReduceMeshArtifact(source, 10u);

    ASSERT_TRUE(reduced.has_value());
    EXPECT_EQ(reduced->indices, source.indices);
    EXPECT_EQ(reduced->vertices.size(), source.vertices.size());
    EXPECT_EQ(reduced->materialIndex, source.materialIndex);
}

TEST(StaticMeshLODTests, MeshReductionIsDeterministic)
{
    const std::vector<NLS::Render::Geometry::Vertex> vertices {
        {{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{1.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{1.0f, 1.0f, 0.0f}, {}, {}, {}, {}},
        {{0.0f, 1.0f, 0.0f}, {}, {}, {}, {}}
    };
    const NLS::Render::Assets::MeshArtifactData source {
        vertices,
        {0u, 1u, 2u, 0u, 2u, 3u},
        2u};

    const auto first = ReduceMeshArtifact(source, 1u);
    const auto second = ReduceMeshArtifact(source, 1u);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->indices, second->indices);
    ASSERT_EQ(first->vertices.size(), second->vertices.size());
    EXPECT_EQ(
        std::memcmp(
            first->vertices.data(),
            second->vertices.data(),
            first->vertices.size() * sizeof(NLS::Render::Geometry::Vertex)),
        0);
}

TEST(StaticMeshLODTests, StaticMeshBuilderAlwaysEmitsLOD0WithoutLODData)
{
    const NLS::Render::Assets::MeshArtifactData lod0 {
        {{{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
         {{1.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
         {{0.0f, 1.0f, 0.0f}, {}, {}, {}, {}}},
        {0u, 1u, 2u},
        3u};

    const auto result = BuildStaticMeshLODArtifact({}, lod0);

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.bundle.lodResources.size(), 1u);
    EXPECT_EQ(result.bundle.lodResources[0].mesh.indices, lod0.indices);
    EXPECT_EQ(result.bundle.lodResources[0].mesh.materialIndex, 3u);
}

TEST(StaticMeshLODTests, StaticMeshBuilderGeneratesPresetLODsDeterministically)
{
    NLS::Render::Assets::MeshArtifactData lod0;
    lod0.materialIndex = 5u;
    for (uint32_t triangle = 0u; triangle < 8u; ++triangle)
    {
        const float x = static_cast<float>(triangle);
        const uint32_t base = static_cast<uint32_t>(lod0.vertices.size());
        lod0.vertices.push_back({{x, 0.0f, 0.0f}, {}, {}, {}, {}});
        lod0.vertices.push_back({{x + 0.5f, 0.0f, 0.0f}, {}, {}, {}, {}});
        lod0.vertices.push_back({{x, 0.5f, 0.0f}, {}, {}, {}, {}});
        lod0.indices.insert(lod0.indices.end(), {base, base + 1u, base + 2u});
    }

    StaticMeshSourceAsset source;
    source.lodGroup = "SmallProp";
    const auto first = BuildStaticMeshLODArtifact(source, lod0);
    const auto second = BuildStaticMeshLODArtifact(source, lod0);

    ASSERT_TRUE(first.success);
    ASSERT_TRUE(second.success);
    ASSERT_EQ(first.bundle.lodResources.size(), 4u);
    ASSERT_EQ(second.bundle.lodResources.size(), 4u);
    for (size_t index = 0u; index < first.bundle.lodResources.size(); ++index)
    {
        EXPECT_EQ(
            first.bundle.lodResources[index].mesh.indices,
            second.bundle.lodResources[index].mesh.indices);
        EXPECT_EQ(
            first.bundle.lodResources[index].mesh.materialIndex,
            lod0.materialIndex);
    }
    EXPECT_EQ(first.bundle.lodResources[1].mesh.indices.size(), 12u);
    EXPECT_EQ(first.bundle.lodResources[2].mesh.indices.size(), 6u);
    EXPECT_EQ(first.bundle.lodResources[3].mesh.indices.size(), 3u);
}

TEST(StaticMeshLODTests, StaticMeshBuilderPreservesAuthoredLOD)
{
    const NLS::Render::Assets::MeshArtifactData lod0 {
        {{{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
         {{1.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
         {{0.0f, 1.0f, 0.0f}, {}, {}, {}, {}}},
        {0u, 1u, 2u},
        1u};
    const NLS::Render::Assets::MeshArtifactData authored {
        {{{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
         {{2.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
         {{0.0f, 2.0f, 0.0f}, {}, {}, {}, {}}},
        {0u, 1u, 2u},
        8u};

    StaticMeshSourceAsset source;
    source.lodGroup = "SmallProp";
    source.sourceModels = {
        StaticMeshSourceModel {StaticMeshLODSourceKind::Imported, 1.0f},
        StaticMeshSourceModel {StaticMeshLODSourceKind::Authored, 0.4f, authored}};

    const auto result = BuildStaticMeshLODArtifact(source, lod0);

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.bundle.lodResources.size(), 4u);
    EXPECT_EQ(result.bundle.lodResources[1].mesh.materialIndex, 8u);
    EXPECT_EQ(result.bundle.lodResources[1].mesh.vertices[1].position[0], 2.0f);
    EXPECT_EQ(result.bundle.lodResources[1].screenSize, 0.4f);
}

TEST(StaticMeshLODTests, FormalLODSelectionUsesDescendingScreenSizeThresholds)
{
    MeshArtifactBundle bundle;
    bundle.lodResources = {
        {{{}, {0u, 1u, 2u}, 0u}, 1.0f},
        {{{}, {0u, 1u, 2u}, 1u}, 0.5f},
        {{{}, {0u, 1u, 2u}, 2u}, 0.25f}};

    EXPECT_EQ(SelectMeshArtifactLOD(bundle, 1.2f), 0u);
    EXPECT_EQ(SelectMeshArtifactLOD(bundle, 0.75f), 1u);
    EXPECT_EQ(SelectMeshArtifactLOD(bundle, 0.25f), 2u);
    EXPECT_EQ(SelectMeshArtifactLOD(bundle, 0.01f), 2u);
}

TEST(StaticMeshLODTests, BundleFileLoadsAllLODsAndLegacyLoadReturnsLOD0)
{
    MeshArtifactBundle bundle;
    bundle.lodResources = {
        {{{{{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
           {{1.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
           {{0.0f, 1.0f, 0.0f}, {}, {}, {}, {}}}, {0u, 1u, 2u}, 4u}, 1.0f},
        {{{{{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
           {{0.5f, 0.0f, 0.0f}, {}, {}, {}, {}},
           {{0.0f, 0.5f, 0.0f}, {}, {}, {}, {}}}, {0u, 1u, 2u}, 7u}, 0.5f}};
    const auto bytes = SerializeMeshArtifactBundle(bundle);
    ASSERT_FALSE(bytes.empty());

    const auto path = std::filesystem::temp_directory_path() / "nullus-static-mesh-lod-bundle.nmesh";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }

    const auto loadedBundle = LoadMeshArtifactBundle(path);
    const auto legacyLOD0 = LoadMeshArtifact(path);

    ASSERT_TRUE(loadedBundle.has_value());
    ASSERT_EQ(loadedBundle->lodResources.size(), 2u);
    EXPECT_EQ(loadedBundle->lodResources[1].mesh.materialIndex, 7u);
    ASSERT_TRUE(legacyLOD0.has_value());
    EXPECT_EQ(legacyLOD0->materialIndex, 4u);
    std::filesystem::remove(path);
}
}
