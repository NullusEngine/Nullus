#include <gtest/gtest.h>

#include "Guid.h"
#include "Rendering/ShaderLab/ShaderLabAsset.h"
#include "Rendering/ShaderLab/ShaderLabMaterial.h"

#include <algorithm>

namespace
{
    NLS::Render::ShaderLab::ShaderLabAssetDesc MakeMaterialShaderDesc()
    {
        using namespace NLS::Render::ShaderLab;

        ShaderLabAssetDesc desc;
        desc.shaderName = "Nullus/MaterialTest";
        desc.properties.push_back({
            "_BaseColor",
            "Base Color",
            ShaderLabPropertyType::Color,
            ShaderLabFloat4{1.0f, 0.5f, 0.25f, 1.0f}
        });
        desc.properties.push_back({
            "_Roughness",
            "Roughness",
            ShaderLabPropertyType::Range,
            0.7f,
            0.0f,
            1.0f
        });
        desc.properties.push_back({
            "_Mode",
            "Mode",
            ShaderLabPropertyType::Int,
            int32_t{1}
        });
        desc.properties.push_back({
            "_TilingOffset",
            "Tiling Offset",
            ShaderLabPropertyType::Vector,
            ShaderLabFloat4{1.0f, 1.0f, 0.0f, 0.0f}
        });
        desc.properties.push_back({
            "_BaseMap",
            "Base Map",
            ShaderLabPropertyType::Texture2D,
            std::string("white")
        });
        return desc;
    }
}

TEST(ShaderLabMaterialTests, BuildsDefaultsAndSupportsTypedSetGet)
{
    const auto shader = std::make_shared<NLS::Render::ShaderLab::ShaderLabAsset>(
        NLS::Guid::NewDeterministic("material-test"),
        MakeMaterialShaderDesc());

    auto material = std::make_shared<NLS::Render::ShaderLab::ShaderLabMaterial>(shader);

    EXPECT_FLOAT_EQ(material->GetFloat("_Roughness").value(), 0.7f);
    EXPECT_EQ(material->GetInt("_Mode").value(), 1);
    EXPECT_EQ(material->GetTexture("_BaseMap").value(), "white");
    EXPECT_FLOAT_EQ(material->GetColor("_BaseColor").value()[2], 0.25f);
    EXPECT_FLOAT_EQ(material->GetVector("_TilingOffset").value()[0], 1.0f);

    EXPECT_TRUE(material->SetFloat("_Roughness", 0.5f));
    EXPECT_TRUE(material->SetInt("_Mode", 3));
    EXPECT_TRUE(material->SetVector("_TilingOffset", {2.0f, 2.0f, 0.5f, 0.25f}));
    EXPECT_TRUE(material->SetTexture("_BaseMap", "Textures/Albedo.png"));
    EXPECT_TRUE(material->SetColor("_BaseColor", {0.1f, 0.2f, 0.3f, 1.0f}));

    EXPECT_FLOAT_EQ(material->GetFloat("_Roughness").value(), 0.5f);
    EXPECT_EQ(material->GetInt("_Mode").value(), 3);
    EXPECT_EQ(material->GetTexture("_BaseMap").value(), "Textures/Albedo.png");
    EXPECT_FLOAT_EQ(material->GetColor("_BaseColor").value()[0], 0.1f);
    EXPECT_FLOAT_EQ(material->GetVector("_TilingOffset").value()[2], 0.5f);
}

TEST(ShaderLabMaterialTests, RejectsTypeMismatchesAndUnknownProperties)
{
    const auto shader = std::make_shared<NLS::Render::ShaderLab::ShaderLabAsset>(
        NLS::Guid::NewDeterministic("material-type-mismatch"),
        MakeMaterialShaderDesc());
    auto material = std::make_shared<NLS::Render::ShaderLab::ShaderLabMaterial>(shader);

    EXPECT_FALSE(material->SetFloat("_BaseMap", 1.0f));
    EXPECT_FALSE(material->SetTexture("_Roughness", "bad"));
    EXPECT_FALSE(material->SetFloat("_Missing", 1.0f));
    EXPECT_FALSE(material->GetFloat("_BaseMap").has_value());
    EXPECT_FALSE(material->GetTexture("_Missing").has_value());
}

TEST(ShaderLabMaterialTests, MigratesValuesByNameAndPreservesOrphansOnShaderReload)
{
    using namespace NLS::Render::ShaderLab;

    const auto oldShader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("old-material-shader"),
        MakeMaterialShaderDesc());
    auto material = std::make_shared<ShaderLabMaterial>(oldShader);
    ASSERT_TRUE(material->SetFloat("_Roughness", 0.25f));
    ASSERT_TRUE(material->SetTexture("_BaseMap", "Textures/Old.png"));

    auto newDesc = MakeMaterialShaderDesc();
    newDesc.properties.erase(
        std::remove_if(
            newDesc.properties.begin(),
            newDesc.properties.end(),
            [](const ShaderLabPropertyDesc& property)
            {
                return property.name == "_BaseMap";
            }),
        newDesc.properties.end());
    newDesc.properties.push_back({
        "_Metallic",
        "Metallic",
        ShaderLabPropertyType::Float,
        0.9f
    });
    const auto newShader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("new-material-shader"),
        newDesc);

    material->ReloadShader(newShader);

    EXPECT_FLOAT_EQ(material->GetFloat("_Roughness").value(), 0.25f);
    EXPECT_FLOAT_EQ(material->GetFloat("_Metallic").value(), 0.9f);
    EXPECT_FALSE(material->GetTexture("_BaseMap").has_value());
    EXPECT_EQ(material->GetOrphanPropertyNames(), std::vector<std::string>({"_BaseMap"}));
    EXPECT_EQ(material->GetOrphanValue("_BaseMap").value().AsTextureName(), "Textures/Old.png");
}

TEST(ShaderLabMaterialTests, FallsBackToDefaultWhenPropertyTypeChangesOnReload)
{
    using namespace NLS::Render::ShaderLab;

    const auto oldShader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("type-change-old"),
        MakeMaterialShaderDesc());
    auto material = std::make_shared<ShaderLabMaterial>(oldShader);
    ASSERT_TRUE(material->SetFloat("_Roughness", 0.25f));

    auto newDesc = MakeMaterialShaderDesc();
    for (auto& property : newDesc.properties)
    {
        if (property.name == "_Roughness")
        {
            property.type = ShaderLabPropertyType::Color;
            property.defaultValue = ShaderLabFloat4{0.9f, 0.8f, 0.7f, 1.0f};
        }
    }

    const auto newShader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("type-change-new"),
        newDesc);
    material->ReloadShader(newShader);

    EXPECT_FLOAT_EQ(material->GetColor("_Roughness").value()[0], 0.9f);
    EXPECT_FALSE(material->GetFloat("_Roughness").has_value());
}

TEST(ShaderLabMaterialTests, ReappearingOrphanWithDifferentTypeUsesDefaultAndClearsOrphan)
{
    using namespace NLS::Render::ShaderLab;

    const auto oldShader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("orphan-type-old"),
        MakeMaterialShaderDesc());
    auto material = std::make_shared<ShaderLabMaterial>(oldShader);
    ASSERT_TRUE(material->SetTexture("_BaseMap", "Textures/Old.png"));

    auto withoutTexture = MakeMaterialShaderDesc();
    withoutTexture.properties.erase(
        std::remove_if(
            withoutTexture.properties.begin(),
            withoutTexture.properties.end(),
            [](const ShaderLabPropertyDesc& property)
            {
                return property.name == "_BaseMap";
            }),
        withoutTexture.properties.end());
    material->ReloadShader(std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("orphan-type-mid"),
        withoutTexture));
    ASSERT_EQ(material->GetOrphanPropertyNames(), std::vector<std::string>({"_BaseMap"}));

    auto reappearingAsColor = MakeMaterialShaderDesc();
    for (auto& property : reappearingAsColor.properties)
    {
        if (property.name == "_BaseMap")
        {
            property.type = ShaderLabPropertyType::Color;
            property.defaultValue = ShaderLabFloat4{0.4f, 0.5f, 0.6f, 1.0f};
        }
    }

    material->ReloadShader(std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("orphan-type-new"),
        reappearingAsColor));

    ASSERT_TRUE(material->GetOrphanPropertyNames().empty());
    EXPECT_FLOAT_EQ(material->GetColor("_BaseMap").value()[1], 0.5f);
    EXPECT_FALSE(material->GetTexture("_BaseMap").has_value());
}

TEST(ShaderLabMaterialTests, KeywordSetIsOrderIndependentAndToggleable)
{
    const auto shader = std::make_shared<NLS::Render::ShaderLab::ShaderLabAsset>(
        NLS::Guid::NewDeterministic("material-keywords"),
        MakeMaterialShaderDesc());
    auto material = std::make_shared<NLS::Render::ShaderLab::ShaderLabMaterial>(shader);

    material->EnableKeyword("_NORMALMAP");
    material->EnableKeyword("_ALPHATEST_ON");
    material->EnableKeyword("_NORMALMAP");

    EXPECT_TRUE(material->IsKeywordEnabled("_NORMALMAP"));
    EXPECT_TRUE(material->IsKeywordEnabled("_ALPHATEST_ON"));
    EXPECT_EQ(material->GetKeywords().ToVector(), std::vector<std::string>({"_ALPHATEST_ON", "_NORMALMAP"}));

    material->DisableKeyword("_NORMALMAP");
    EXPECT_FALSE(material->IsKeywordEnabled("_NORMALMAP"));
    EXPECT_TRUE(material->IsKeywordEnabled("_ALPHATEST_ON"));
}
