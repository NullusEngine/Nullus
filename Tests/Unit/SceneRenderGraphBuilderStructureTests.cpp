#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return {};

        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
}

TEST(SceneRenderGraphBuilderStructureTests, BuilderFacadeIncludesSeparatedForwardDeferredAndLightGridModules)
{
    const std::filesystem::path root = "Runtime/Rendering/FrameGraph";

    EXPECT_TRUE(std::filesystem::exists(root / "SceneRenderGraphBuilderForward.h"));
    EXPECT_TRUE(std::filesystem::exists(root / "SceneRenderGraphBuilderDeferred.h"));
    EXPECT_TRUE(std::filesystem::exists(root / "SceneRenderGraphBuilderLightGrid.h"));
    EXPECT_TRUE(std::filesystem::exists(root / "SceneRenderGraphBuilderInternal.h"));

    const auto facade = ReadTextFile(root / "SceneRenderGraphBuilder.h");
    EXPECT_NE(facade.find("SceneRenderGraphBuilderForward.h"), std::string::npos);
    EXPECT_NE(facade.find("SceneRenderGraphBuilderDeferred.h"), std::string::npos);
    EXPECT_NE(facade.find("SceneRenderGraphBuilderLightGrid.h"), std::string::npos);
}
