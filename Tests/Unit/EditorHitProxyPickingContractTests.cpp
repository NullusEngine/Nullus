#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    std::string ReadRepositoryTextFile(const std::filesystem::path& relativePath)
    {
        const auto absolutePath = std::filesystem::path(NLS_ROOT_DIR) / relativePath;
        std::ifstream input(absolutePath, std::ios::binary);
        EXPECT_TRUE(input.is_open()) << absolutePath.string();

        std::ostringstream output;
        output << input.rdbuf();
        return output.str();
    }
}

TEST(EditorHitProxyPickingContractTests, PickingRenderPassReusesHoverFramesButRebuildsClicks)
{
    const std::string source = ReadRepositoryTextFile(
        "Project/Editor/Rendering/PickingRenderPass.cpp");

    EXPECT_NE(source.find("!debugSceneDescriptor.requestPickingFrameForClick"), std::string::npos);
    EXPECT_NE(source.find("CanReuseReadablePickingFrameForSignature(pickingSignature)"), std::string::npos);
    EXPECT_NE(source.find("EditorPicking::Reuse"), std::string::npos);
    EXPECT_NE(source.find("EditorPicking::Rebuild"), std::string::npos);
}

TEST(EditorHitProxyPickingContractTests, HoverBudgetSkipRunsBeforeExpensivePickingSignatureBuild)
{
    const std::string source = ReadRepositoryTextFile(
        "Project/Editor/Rendering/PickingRenderPass.cpp");

    const auto skip = source.find("ShouldSkipSceneHoverPickingForVisibleDrawBudget");
    const auto signature = source.find("BuildCurrentPickingSignature(sceneDescriptor.scene)");
    ASSERT_NE(skip, std::string::npos);
    ASSERT_NE(signature, std::string::npos);
    EXPECT_LT(skip, signature);
}

TEST(EditorHitProxyPickingContractTests, PickingRebuildInvalidatesStaleCompletedReadbackBeforeSubmittingFrame)
{
    const std::string source = ReadRepositoryTextFile(
        "Project/Editor/Rendering/PickingRenderPass.cpp");

    const auto rebuild = source.find("EditorPicking::Rebuild");
    const auto invalidate = source.find("InvalidateCompletedReadbackTexture", rebuild);
    const auto queue = source.find("QueueSubmittedFrame", rebuild);
    const auto immediate = source.find("MarkSubmittedFrameImmediatelyReadable", rebuild);
    ASSERT_NE(rebuild, std::string::npos);
    ASSERT_NE(invalidate, std::string::npos);
    ASSERT_NE(queue, std::string::npos);
    ASSERT_NE(immediate, std::string::npos);
    EXPECT_LT(invalidate, queue);
    EXPECT_LT(invalidate, immediate);
}

TEST(EditorHitProxyPickingContractTests, PickingSignatureIncludesCameraAndLightHelpers)
{
    const std::string source = ReadRepositoryTextFile(
        "Project/Editor/Rendering/PickingRenderPass.cpp");

    const auto signature = source.find("BuildCurrentPickingSignature");
    ASSERT_NE(signature, std::string::npos);
    const auto body = source.substr(signature, source.find("CanReuseReadablePickingFrameForSignature", signature) - signature);
    EXPECT_NE(body.find("fastAccess.cameras"), std::string::npos);
    EXPECT_NE(body.find("fastAccess.lights"), std::string::npos);
    EXPECT_NE(body.find("HashPickableActor"), std::string::npos);
}

TEST(EditorHitProxyPickingContractTests, HoverBudgetSkipDoesNotApplyToClickPicking)
{
    const std::string source = ReadRepositoryTextFile(
        "Project/Editor/Rendering/PickingRenderPass.cpp");
    const std::string policy = ReadRepositoryTextFile(
        "Project/Editor/Panels/SceneViewPickingPolicy.h");

    EXPECT_NE(source.find("debugSceneDescriptor.requestPickingFrameForClick"), std::string::npos);
    EXPECT_NE(source.find("EditorPicking::SkipHoverBudget"), std::string::npos);
    EXPECT_NE(policy.find("requestKind == HitProxyPickingRequestKind::Hover"), std::string::npos);
}

TEST(EditorHitProxyPickingContractTests, SceneViewExposesClickResolveAndReadbackWaitScopes)
{
    const std::string source = ReadRepositoryTextFile(
        "Project/Editor/Panels/SceneView.cpp");

    EXPECT_NE(source.find("EditorPicking::ResolveClick"), std::string::npos);
    EXPECT_NE(source.find("EditorPicking::WaitReadback"), std::string::npos);
    EXPECT_NE(source.find("CanResolvePickingRequest"), std::string::npos);
    EXPECT_EQ(source.find("ShouldResolvePendingSceneClickPick("), std::string::npos);
}

TEST(EditorHitProxyPickingContractTests, SelectionOutlinePathStaysSeparateFromPickingReadback)
{
    const std::string header = ReadRepositoryTextFile(
        "Project/Editor/Rendering/SelectionOutlineMaskRenderer.h");
    const std::string source = ReadRepositoryTextFile(
        "Project/Editor/Rendering/SelectionOutlineMaskRenderer.cpp");

    EXPECT_NE(header.find("SelectionOutlineMaskRenderer"), std::string::npos);
    EXPECT_NE(header.find("SelectionOutlineMask::Composite"), std::string::npos);
    EXPECT_NE(source.find("SelectionOutlineMask::CaptureMask"), std::string::npos);
    EXPECT_NE(source.find("SelectionOutlineMask::RecordComposite"), std::string::npos);
    EXPECT_EQ(header.find("PickingReadbackLifecycle"), std::string::npos);
    EXPECT_EQ(source.find("PickingReadbackLifecycle"), std::string::npos);
}
