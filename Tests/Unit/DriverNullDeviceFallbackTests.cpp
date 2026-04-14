#include <gtest/gtest.h>

#include "Rendering/Context/Driver.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

TEST(DriverNullDeviceFallbackTests, ReturnsSafeFallbackDecisionsWhenExplicitDeviceIsMissing)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);

    const auto editorDecision = driver.EvaluateEditorMainRuntimeFallback(
        NLS::Render::Settings::EGraphicsBackend::DX12);
    EXPECT_FALSE(editorDecision.shouldFallbackToOpenGL);
    ASSERT_TRUE(editorDecision.primaryWarning.has_value());
    EXPECT_NE(editorDecision.primaryWarning->find("no validated fallback backend"), std::string::npos);
    ASSERT_TRUE(editorDecision.detailWarning.has_value());

    const auto gameDecision = driver.EvaluateGameMainRuntimeFallback(
        NLS::Render::Settings::EGraphicsBackend::VULKAN);
    EXPECT_FALSE(gameDecision.shouldFallbackToOpenGL);
    ASSERT_TRUE(gameDecision.primaryWarning.has_value());
    EXPECT_NE(gameDecision.primaryWarning->find("no validated fallback backend"), std::string::npos);
    ASSERT_TRUE(gameDecision.detailWarning.has_value());
}

TEST(DriverNullDeviceFallbackTests, ReportsNoneBackendAndPickingWarningWithoutExplicitDevice)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);

    EXPECT_EQ(driver.GetActiveGraphicsBackend(), NLS::Render::Settings::EGraphicsBackend::NONE);

    const auto warning = driver.GetEditorPickingReadbackWarning();
    ASSERT_TRUE(warning.has_value());
    EXPECT_NE(warning->find("Scene view picking readback is unavailable"), std::string::npos);
}
