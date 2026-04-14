#include <gtest/gtest.h>

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h"

TEST(DX12ReadbackUtilsTests, AlignsSinglePixelReadbackRowsToDx12PitchRequirement)
{
    const auto layout = NLS::Render::RHI::DX12::BuildDX12ReadbackLayout(DXGI_FORMAT_R8G8B8A8_UNORM, 1u, 1u);

    EXPECT_EQ(layout.bytesPerPixel, 4u);
    EXPECT_EQ(layout.rowPitch, 256u);
    EXPECT_EQ(layout.readbackSize, 256u);
}

TEST(DX12ReadbackUtilsTests, PreservesAlreadyAlignedRowPitch)
{
    const auto layout = NLS::Render::RHI::DX12::BuildDX12ReadbackLayout(DXGI_FORMAT_R8G8B8A8_UNORM, 64u, 2u);

    EXPECT_EQ(layout.bytesPerPixel, 4u);
    EXPECT_EQ(layout.rowPitch, 256u);
    EXPECT_EQ(layout.readbackSize, 512u);
}

TEST(DX12ReadbackUtilsTests, UsesFormatByteSizeWhenComputingReadbackFootprint)
{
    const auto layout = NLS::Render::RHI::DX12::BuildDX12ReadbackLayout(DXGI_FORMAT_R16G16B16A16_FLOAT, 3u, 1u);

    EXPECT_EQ(layout.bytesPerPixel, 8u);
    EXPECT_EQ(layout.rowPitch, 256u);
    EXPECT_EQ(layout.readbackSize, 256u);
}
#endif
