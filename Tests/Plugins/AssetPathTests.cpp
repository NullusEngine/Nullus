#include <gtest/gtest.h>
#include "Core/Assets/PluginAssetPath.h"
using namespace NLS::Core::Assets;
TEST(PluginAssetIdentity, NormalizesUnicodeAndSeparatorsWithoutDependingOnHost)
{
    EXPECT_EQ(NormalizePluginAssetPath("Textures\\./Cafe\xcc\x81.png"), "Textures/Caf\xc3\xa9.png");
    EXPECT_EQ(NormalizePluginAssetPath("\xe1\x84\x80\xe1\x85\xa1.png"), "\xea\xb0\x80.png");
    EXPECT_EQ(PluginAssetCollisionKey("Stra\xc3\x9f" "e.PNG"), PluginAssetCollisionKey("STRASSE.png"));
    EXPECT_EQ(PluginAssetGuid("org.a","content","Cafe\xcc\x81.png"), PluginAssetGuid("org.a","content","Caf\xc3\xa9.png"));
    EXPECT_NE(PluginAssetGuid("org.a","content","x.png"), PluginAssetGuid("org.b","content","x.png"));
    EXPECT_THROW(NormalizePluginAssetPath("../outside"),std::exception);
    EXPECT_THROW(NormalizePluginAssetPath("C:/outside"),std::exception);
    EXPECT_THROW(NormalizePluginAssetPath("bad\xc0\xaf"),std::exception);
}
