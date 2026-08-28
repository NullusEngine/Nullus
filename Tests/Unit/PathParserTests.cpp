#include <gtest/gtest.h>

#include "Utils/PathParser.h"

TEST(PathParserTests, TreatsNativeMaterialArtifactsAsMaterialFiles)
{
    EXPECT_EQ(
        NLS::Utils::PathParser::GetFileType("Assets/Materials/Hero.mat"),
        NLS::Utils::PathParser::EFileType::MATERIAL);
}

TEST(PathParserTests, TreatsShaderLabAssetsAsShaderFiles)
{
    EXPECT_EQ(
        NLS::Utils::PathParser::GetFileType("Assets/Shaders/ShaderLab/Hero.shader"),
        NLS::Utils::PathParser::EFileType::SHADER);
    EXPECT_EQ(
        NLS::Utils::PathParser::GetFileType("Assets/Shaders/ShaderLab/Hero.shadet"),
        NLS::Utils::PathParser::EFileType::UNKNOWN);
}

TEST(PathParserTests, ClassifiesAssetBrowserProjectExtensionsConsistently)
{
    using FileType = NLS::Utils::PathParser::EFileType;

    struct Case
    {
        const char* path;
        FileType expected;
    };

    const Case cases[] {
        {"Assets/Models/Hero.fbx", FileType::MODEL},
        {"Assets/Models/Hero.gltf", FileType::MODEL},
        {"Assets/Models/Hero.glb", FileType::MODEL},
        {"Assets/Textures/Hero.png", FileType::TEXTURE},
        {"Assets/Textures/Hero.bmp", FileType::TEXTURE},
        {"Assets/Textures/Hero.dds", FileType::TEXTURE},
        {"Assets/Shaders/Hero.shader", FileType::SHADER},
        {"Assets/Scenes/Hero.scene", FileType::SCENE},
        {"Assets/Scenes/Hero.objectgraph.json", FileType::SCENE},
        {"Assets/Scripts/Hero.cs", FileType::SCRIPT},
        {"Assets/Scripts/Hero.py", FileType::SCRIPT},
        {"Assets/Materials/Hero.mat", FileType::MATERIAL},
        {"Assets/UI/HUD.ui", FileType::UI_DOCUMENT},
        {"Assets/UI/Theme.style", FileType::UI_STYLE},
        {"Assets/Localization/Strings.loc", FileType::LOCALIZATION},
        {"Assets/Fonts/NotoSans.woff2", FileType::FONT}
    };

    for (const auto& testCase : cases)
    {
        EXPECT_EQ(
            NLS::Utils::PathParser::GetFileType(testCase.path),
            testCase.expected) << testCase.path;
    }
}

TEST(PathParserTests, DoesNotTreatLegacyPrefixedUiExtensionsAsNativeAssetTypes)
{
    using FileType = NLS::Utils::PathParser::EFileType;
    EXPECT_EQ(NLS::Utils::PathParser::GetFileType("Assets/UI/HUD.nui"), FileType::UNKNOWN);
    EXPECT_EQ(NLS::Utils::PathParser::GetFileType("Assets/UI/Theme.nstyle"), FileType::UNKNOWN);
    EXPECT_EQ(NLS::Utils::PathParser::GetFileType("Assets/Localization/Strings.nloc"), FileType::UNKNOWN);
}
