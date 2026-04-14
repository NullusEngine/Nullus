#include <gtest/gtest.h>

#include "Rendering/FrameGraphSceneTargets.h"
#include "Rendering/RHI/Core/RHIEnums.h"

TEST(FrameGraphSceneTargetsTests, SceneColorTargetSupportsSamplingForEditorViews)
{
    const auto desc = NLS::Engine::Rendering::MakeSceneColorTargetDesc(1280, 720);

    EXPECT_EQ(desc.extent.width, 1280u);
    EXPECT_EQ(desc.extent.height, 720u);
    EXPECT_EQ(desc.format, NLS::Render::RHI::TextureFormat::RGB8);
    EXPECT_TRUE(NLS::Render::RHI::HasTextureUsage(
        desc.usage,
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment));
    EXPECT_TRUE(NLS::Render::RHI::HasTextureUsage(
        desc.usage,
        NLS::Render::RHI::TextureUsageFlags::Sampled));
}
