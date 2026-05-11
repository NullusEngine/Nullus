#include <gtest/gtest.h>

#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/MaterialVariantKey.h"

TEST(MaterialVariantKeyTests, BuildsStableMaterialIdentityFromAssetPath)
{
    NLS::Render::Resources::Material firstMaterial;
    NLS::Render::Resources::Material secondMaterial;
    const_cast<std::string&>(firstMaterial.path) = "App/Assets/Shared.nmat";
    const_cast<std::string&>(secondMaterial.path) = "App/Assets/Shared.nmat";
    secondMaterial.SetBlendable(true);
    secondMaterial.SetColorWriting(false);

    const auto firstIdentity = NLS::Render::Resources::BuildMaterialVariantIdentity(firstMaterial);
    const auto secondIdentity = NLS::Render::Resources::BuildMaterialVariantIdentity(secondMaterial);

    EXPECT_EQ(firstIdentity.stableKey, "path:App/Assets/Shared.nmat");
    EXPECT_EQ(secondIdentity.stableKey, firstIdentity.stableKey);
}

TEST(MaterialVariantKeyTests, BuildsRuntimeMaterialIdentityFromShaderAndRenderState)
{
    NLS::Render::Resources::Material material;
    material.SetDepthTest(false);
    material.SetDepthWriting(false);
    material.SetColorWriting(false);
    material.SetBlendable(true);
    material.SetBackfaceCulling(false);
    material.SetFrontfaceCulling(true);

    const auto identity = NLS::Render::Resources::BuildMaterialVariantIdentity(material);

    EXPECT_NE(identity.stableKey.find("runtime:"), std::string::npos);
    EXPECT_NE(identity.stableKey.find("|depthTest:0"), std::string::npos);
    EXPECT_NE(identity.stableKey.find("|depthWrite:0"), std::string::npos);
    EXPECT_NE(identity.stableKey.find("|colorWrite:0"), std::string::npos);
    EXPECT_NE(identity.stableKey.find("|blend:1"), std::string::npos);
    EXPECT_NE(identity.stableKey.find("|backCull:0"), std::string::npos);
    EXPECT_NE(identity.stableKey.find("|frontCull:1"), std::string::npos);
}

TEST(MaterialVariantKeyTests, BuildsPassAndPipelineVariantKeysFromMaterialPipelineAndOverrides)
{
    NLS::Render::Resources::Material material;
    const_cast<std::string&>(material.path) = "App/Assets/Variant.nmat";

    NLS::Render::Data::PipelineState pipelineState;
    pipelineState.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::GREATER;
    pipelineState.blending = true;

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.depthTest = false;
    overrides.colorWrite = true;
    overrides.culling = false;

    const auto gbufferKey = NLS::Render::Resources::BuildMaterialPassVariantKey(
        material,
        "DeferredGBuffer",
        pipelineState,
        overrides);
    const auto lightingKey = NLS::Render::Resources::BuildMaterialPassVariantKey(
        material,
        "DeferredLighting",
        pipelineState,
        overrides);

    auto changedPipelineState = pipelineState;
    changedPipelineState.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::LESS;
    const auto changedPipelineKey = NLS::Render::Resources::BuildMaterialPassVariantKey(
        material,
        "DeferredGBuffer",
        changedPipelineState,
        overrides);

    EXPECT_NE(gbufferKey.stableKey.find("pass:DeferredGBuffer"), std::string::npos);
    EXPECT_NE(gbufferKey.stableKey.find("material:path:App/Assets/Variant.nmat"), std::string::npos);
    EXPECT_NE(gbufferKey.stableKey.find("overrideDepthTest:0"), std::string::npos);
    EXPECT_NE(gbufferKey.stableKey, lightingKey.stableKey);
    EXPECT_NE(gbufferKey.stableKey, changedPipelineKey.stableKey);
}

TEST(MaterialVariantKeyTests, IncludesRenderTargetFormatsInPassVariantKey)
{
    NLS::Render::Resources::Material material;
    const_cast<std::string&>(material.path) = "App/Assets/GBuffer.nmat";

    const NLS::Render::Data::PipelineState pipelineState;

    NLS::Render::Resources::MaterialPipelineStateOverrides singleTargetOverrides;
    singleTargetOverrides.colorFormats = {
        NLS::Render::RHI::TextureFormat::RGBA8
    };

    NLS::Render::Resources::MaterialPipelineStateOverrides gbufferOverrides;
    gbufferOverrides.colorFormats = {
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::RHI::TextureFormat::RGBA8
    };

    const auto singleTargetKey = NLS::Render::Resources::BuildMaterialPassVariantKey(
        material,
        "DeferredGBuffer",
        pipelineState,
        singleTargetOverrides);
    const auto gbufferKey = NLS::Render::Resources::BuildMaterialPassVariantKey(
        material,
        "DeferredGBuffer",
        pipelineState,
        gbufferOverrides);

    EXPECT_NE(singleTargetKey.stableKey, gbufferKey.stableKey);
    EXPECT_NE(gbufferKey.stableKey.find("overrideColorFormats:3"), std::string::npos);
}
