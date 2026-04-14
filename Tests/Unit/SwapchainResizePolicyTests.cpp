#include <chrono>

#include <gtest/gtest.h>

#include "Rendering/Context/SwapchainResizePolicy.h"

using namespace std::chrono_literals;

TEST(SwapchainResizePolicyTests, InteractiveResizeUsesNoDebounceByDefault)
{
    EXPECT_EQ(NLS::Render::Context::GetInteractiveSwapchainResizeDebounce(), 0ms);
    EXPECT_TRUE(NLS::Render::Context::ShouldApplyPendingSwapchainResize(0ms));
}

TEST(SwapchainResizePolicyTests, HonorsCustomDebounceWhenExplicitlyProvided)
{
    EXPECT_FALSE(NLS::Render::Context::ShouldApplyPendingSwapchainResize(10ms, 100ms));
    EXPECT_TRUE(NLS::Render::Context::ShouldApplyPendingSwapchainResize(100ms, 100ms));
    EXPECT_TRUE(NLS::Render::Context::ShouldApplyPendingSwapchainResize(150ms, 100ms));
}
