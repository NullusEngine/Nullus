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
        {"Assets/Materials/Hero.mat", FileType::MATERIAL}
    };

    for (const auto& testCase : cases)
    {
        EXPECT_EQ(
            NLS::Utils::PathParser::GetFileType(testCase.path),
            testCase.expected) << testCase.path;
    }
}
