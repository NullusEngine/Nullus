#include <gtest/gtest.h>

#include <chrono>

#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/RenderScenePackage.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Tests/RenderingThreadTestHelpers.h"

TEST(DriverNullDeviceReadinessTests, DestructionUnregistersTheLocatedDriverBeforeBackendFailureNotifications)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    for (size_t iteration = 0u; iteration < 2u; ++iteration)
    {
        {
            Driver driver(settings);
            NLS::Core::ServiceLocator::Provide(driver);
            EXPECT_EQ(TryGetLocatedDriver(), &driver);
        }
        EXPECT_FALSE(NLS::Core::ServiceLocator::Contains<Driver>());
        EXPECT_EQ(TryGetLocatedDriver(), nullptr);
        EXPECT_NO_THROW(MarkLocatedDriverUnsafeGpuWorkQuarantined("No live driver owns this failure"));
        EXPECT_NO_THROW(MarkLocatedDriverDeviceLost("No live driver owns this device"));
    }
}

TEST(DriverNullDeviceReadinessTests, ReturnsSafeReadinessDecisionsWhenExplicitDeviceIsMissing)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);

    const auto requiredBackend = NLS::Render::Settings::GetPhase1RequiredRuntimeBackend();
    const auto editorDecision = driver.EvaluateEditorMainRuntimeReadiness(requiredBackend);
    ASSERT_TRUE(editorDecision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(requiredBackend))
        EXPECT_NE(editorDecision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(editorDecision.primaryWarning->find(
            "only supports " + std::string(NLS::Render::Settings::ToString(requiredBackend))), std::string::npos);
    ASSERT_TRUE(editorDecision.detailWarning.has_value());

    const auto gameDecision = driver.EvaluateGameMainRuntimeReadiness(requiredBackend);
    ASSERT_TRUE(gameDecision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(requiredBackend))
        EXPECT_NE(gameDecision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(gameDecision.primaryWarning->find(
            "only supports " + std::string(NLS::Render::Settings::ToString(requiredBackend))), std::string::npos);
    ASSERT_TRUE(gameDecision.detailWarning.has_value());
}

TEST(DriverNullDeviceReadinessTests, ReportsNoneBackendAndPickingWarningWithoutExplicitDevice)
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

TEST(DriverNullDeviceReadinessTests, ThreadedOffscreenOnlyFramesRetireWithoutExplicitDeviceOrSwapchain)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto submission = NLS::Tests::ObserveNextRhiFrameForTest(driver);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 23u;
    snapshot.targetsSwapchain = false;

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver, snapshot, [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));
    ASSERT_EQ(submission.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto frame = submission.get();
    EXPECT_EQ(frame.frameId, 23u);
    EXPECT_TRUE(frame.offscreenOnly);
}

TEST(DriverNullDeviceReadinessTests, DX12NullDeviceDoesNotExposeDirectExplicitLegacyPath)
{
    const auto requiredBackend = NLS::Render::Settings::GetPhase1RequiredRuntimeBackend();
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = requiredBackend;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, nullptr);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, nullptr);

    EXPECT_EQ(driver.GetActiveGraphicsBackend(), NLS::Render::Settings::EGraphicsBackend::NONE);
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(driver));
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    EXPECT_FALSE(NLS::Render::Context::DriverUIAccess::PublishUiOnlyFrame(driver, 128u, 72u));

    const auto editorDecision = driver.EvaluateEditorMainRuntimeReadiness(requiredBackend);
    ASSERT_TRUE(editorDecision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(requiredBackend))
        EXPECT_NE(editorDecision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(editorDecision.primaryWarning->find(
            "only supports " + std::string(NLS::Render::Settings::ToString(requiredBackend))), std::string::npos);
    ASSERT_TRUE(editorDecision.detailWarning.has_value());
    EXPECT_NE(
        editorDecision.detailWarning->find(
            requiredBackend == NLS::Render::Settings::EGraphicsBackend::VULKAN
                ? "active runtime backend"
                : "only active runtime backend"),
        std::string::npos);

    const auto gameDecision = driver.EvaluateGameMainRuntimeReadiness(requiredBackend);
    ASSERT_TRUE(gameDecision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(requiredBackend))
        EXPECT_NE(gameDecision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(gameDecision.primaryWarning->find(
            "only supports " + std::string(NLS::Render::Settings::ToString(requiredBackend))), std::string::npos);
    ASSERT_TRUE(gameDecision.detailWarning.has_value());
    EXPECT_NE(
        gameDecision.detailWarning->find(
            requiredBackend == NLS::Render::Settings::EGraphicsBackend::VULKAN
                ? "active runtime backend"
                : "only active runtime backend"),
        std::string::npos);
}

TEST(DriverNullDeviceReadinessTests, DX12NullDeviceRejectsRenderFrameCommands)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, nullptr);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, nullptr);

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 77u;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = 77u;
    renderScenePackage.visibleDrawCount = 1u;
    renderScenePackage.hasVisibleDraws = true;

    bool builderCalled = false;
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        snapshot,
        [&](size_t) { builderCalled = true; return renderScenePackage; }));

    EXPECT_FALSE(builderCalled);
    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.inFlightFrameCount, 0u);
    EXPECT_EQ(telemetry.publishedFrameCount, 0u);
}

TEST(DriverNullDeviceReadinessTests, NonDx12RequestedBackendDoesNotCreatePhase1RuntimeDevice)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX11;
    settings.framesInFlight = 1;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);

    EXPECT_EQ(driver.GetActiveGraphicsBackend(), NLS::Render::Settings::EGraphicsBackend::NONE);
}
