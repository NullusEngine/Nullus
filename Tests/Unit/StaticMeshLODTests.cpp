#include <gtest/gtest.h>

#include "Rendering/Assets/StaticMeshLODSettings.h"
#include "Rendering/Assets/MeshArtifact.h"

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
}
