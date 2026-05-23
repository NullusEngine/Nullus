#include <gtest/gtest.h>

#include "Assets/EditorAssetPathUtils.h"

TEST(EditorAssetPathUtilsTests, RecognizesBuiltInModelResourcePaths)
{
    EXPECT_TRUE(NLS::Editor::Assets::IsBuiltInResourcePath(":Models\\Cone.fbx"));
    EXPECT_TRUE(NLS::Editor::Assets::IsBuiltInResourcePath(":Models/Cube.fbx"));
    EXPECT_FALSE(NLS::Editor::Assets::IsBuiltInResourcePath("Models/Cone.fbx"));
    EXPECT_FALSE(NLS::Editor::Assets::IsBuiltInResourcePath(""));
}

TEST(EditorAssetPathUtilsTests, FormatsBuiltInResourceDisplayName)
{
    EXPECT_EQ(NLS::Editor::Assets::GetBuiltInResourceDisplayName(":Models\\Cone.fbx"), "Cone");
    EXPECT_EQ(NLS::Editor::Assets::GetBuiltInResourceDisplayName(":Models/Cube.fbx"), "Cube");
    EXPECT_EQ(NLS::Editor::Assets::GetBuiltInResourceDisplayName(":Models"), "Models");
}
