#include <gtest/gtest.h>

#include "Rendering/Settings/GraphicsBackendUtils.h"

TEST(GraphicsBackendUtilsTests, ParsesDX11BackendAliases)
{
    const auto dx11 = NLS::Render::Settings::EGraphicsBackend::DX11;

    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("dx11"), dx11);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("Dx11"), dx11);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("directx11"), dx11);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("d3d11"), dx11);
    EXPECT_EQ(NLS::Render::Settings::TryParseGraphicsBackend("not-a-backend"), std::nullopt);
}

TEST(GraphicsBackendUtilsTests, StringifiesDX11Backend)
{
    EXPECT_STREQ(NLS::Render::Settings::ToString(NLS::Render::Settings::EGraphicsBackend::DX11), "DX11");
}
