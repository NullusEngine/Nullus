#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::string ReadSourceFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}
}

TEST(EditorSelectedBoundsContractTests, SelectedBoundsDebugDrawUsesMeshAabbBoxes)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp";
    const std::string source = ReadSourceFile(sourcePath);

    const auto drawElementsStart = source.find("void DrawGameObjectDebugElements");
    const auto drawElementsEnd = source.find("void DrawFrustumLines", drawElementsStart);
    ASSERT_NE(drawElementsStart, std::string::npos);
    ASSERT_NE(drawElementsEnd, std::string::npos);
    const auto drawElementsBody = source.substr(drawElementsStart, drawElementsEnd - drawElementsStart);

    EXPECT_NE(drawElementsBody.find("DrawMeshBounds(item)"), std::string::npos);
    EXPECT_EQ(drawElementsBody.find("DrawBoundingSpheres"), std::string::npos);

    const auto drawBoundsStart = source.find("void DrawMeshBounds");
    const auto drawBoundsEnd = source.find("private:", drawBoundsStart);
    ASSERT_NE(drawBoundsStart, std::string::npos);
    ASSERT_NE(drawBoundsEnd, std::string::npos);
    const auto drawBoundsBody = source.substr(drawBoundsStart, drawBoundsEnd - drawBoundsStart);

    EXPECT_NE(drawBoundsBody.find("GetBounds()"), std::string::npos);
    EXPECT_NE(drawBoundsBody.find("SubmitBox("), std::string::npos);
    EXPECT_EQ(drawBoundsBody.find("SubmitSphere("), std::string::npos);
    EXPECT_EQ(drawBoundsBody.find("GetBoundingSphere("), std::string::npos);
    EXPECT_EQ(drawBoundsBody.find("GetCustomBoundingSphere("), std::string::npos);
}

TEST(EditorSelectedBoundsContractTests, SelectedMeshSceneViewFilterUsesMeshAabb)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugGameObjectSelectionCollector.h";
    const std::string source = ReadSourceFile(sourcePath);

    const auto filterStart = source.find("inline bool IsSelectedDebugMeshVisibleToSceneView");
    const auto filterEnd = source.find("inline void CollectSelectedDebugGameObjectDebugDrawItems", filterStart);
    ASSERT_NE(filterStart, std::string::npos);
    ASSERT_NE(filterEnd, std::string::npos);
    const auto filterBody = source.substr(filterStart, filterEnd - filterStart);

    EXPECT_NE(filterBody.find("BoundsInFrustum"), std::string::npos);
    EXPECT_NE(filterBody.find("GetBounds()"), std::string::npos);
    EXPECT_EQ(filterBody.find("BoundingSphereInFrustum"), std::string::npos);
    EXPECT_EQ(filterBody.find("GetBoundingSphere("), std::string::npos);
    EXPECT_EQ(filterBody.find("GetCustomBoundingSphere("), std::string::npos);
}

TEST(EditorSelectedBoundsContractTests, SelectedGizmoCenterUsesMeshAabb)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/SceneViewImGuizmo.cpp";
    const std::string source = ReadSourceFile(sourcePath);

    const auto accumulateStart = source.find("bool AccumulateGameObjectWorldBounds");
    const auto accumulateEnd = source.find("void RestoreGameObjectCenterPivotPosition", accumulateStart);
    ASSERT_NE(accumulateStart, std::string::npos);
    ASSERT_NE(accumulateEnd, std::string::npos);
    const auto accumulateBody = source.substr(accumulateStart, accumulateEnd - accumulateStart);

    EXPECT_NE(accumulateBody.find("GetBounds()"), std::string::npos);
    EXPECT_NE(accumulateBody.find("TransformBounds"), std::string::npos);
    EXPECT_EQ(accumulateBody.find("GetBoundingSphere("), std::string::npos);
    EXPECT_EQ(accumulateBody.find("GetCustomBoundingSphere("), std::string::npos);
}

TEST(EditorSelectedBoundsContractTests, CameraFocusDistanceUsesMeshAabb)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/CameraController.cpp";
    const std::string source = ReadSourceFile(sourcePath);

    const auto focusStart = source.find("float GetActorFocusDist");
    const auto focusEnd = source.find("void Editor::Core::CameraController::HandleInputs", focusStart);
    ASSERT_NE(focusStart, std::string::npos);
    ASSERT_NE(focusEnd, std::string::npos);
    const auto focusBody = source.substr(focusStart, focusEnd - focusStart);

    EXPECT_NE(focusBody.find("GetBounds()"), std::string::npos);
    EXPECT_NE(focusBody.find("TransformBounds"), std::string::npos);
    EXPECT_EQ(focusBody.find("BoundingSphere"), std::string::npos);
    EXPECT_EQ(focusBody.find("GetCustomBoundingSphere("), std::string::npos);
}
