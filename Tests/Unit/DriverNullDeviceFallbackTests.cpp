#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

TEST(DriverNullDeviceReadinessTests, ReturnsSafeReadinessDecisionsWhenExplicitDeviceIsMissing)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);

    const auto editorDecision = driver.EvaluateEditorMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::DX12);
    ASSERT_TRUE(editorDecision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(NLS::Render::Settings::EGraphicsBackend::DX12))
        EXPECT_NE(editorDecision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(editorDecision.primaryWarning->find("only supports DX12"), std::string::npos);
    ASSERT_TRUE(editorDecision.detailWarning.has_value());

    const auto gameDecision = driver.EvaluateGameMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::DX12);
    ASSERT_TRUE(gameDecision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(NLS::Render::Settings::EGraphicsBackend::DX12))
        EXPECT_NE(gameDecision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(gameDecision.primaryWarning->find("only supports DX12"), std::string::npos);
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
    settings.framesInFlight = 1;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 23u;
    snapshot.targetsSwapchain = false;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    const NLS::Render::Context::InFlightFrameSlot* retiredSlot = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        retiredSlot = lifecycle->PeekSlot(0u);
        if (retiredSlot != nullptr && retiredSlot->stage == NLS::Render::Context::ThreadedFrameStage::Retired)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_NE(retiredSlot, nullptr);
    EXPECT_EQ(retiredSlot->stage, NLS::Render::Context::ThreadedFrameStage::Retired);
    ASSERT_TRUE(retiredSlot->submissionFrame.has_value());
    EXPECT_EQ(retiredSlot->submissionFrame->frameId, 23u);
    EXPECT_TRUE(retiredSlot->submissionFrame->offscreenOnly);
}

TEST(DriverNullDeviceReadinessTests, DX12NullDeviceDoesNotExposeDirectExplicitLegacyPath)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.framesInFlight = 1;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, nullptr);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, nullptr);

    EXPECT_EQ(driver.GetActiveGraphicsBackend(), NLS::Render::Settings::EGraphicsBackend::NONE);
    EXPECT_FALSE(NLS::Render::Context::DriverTestAccess::CanBeginStandaloneExplicitFrame(driver));
    EXPECT_EQ(NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(driver), nullptr);
    EXPECT_FALSE(NLS::Render::Context::DriverUIAccess::PrepareUIRender(driver));

    const auto editorDecision = driver.EvaluateEditorMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::DX12);
    ASSERT_TRUE(editorDecision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(NLS::Render::Settings::EGraphicsBackend::DX12))
        EXPECT_NE(editorDecision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(editorDecision.primaryWarning->find("only supports DX12"), std::string::npos);
    ASSERT_TRUE(editorDecision.detailWarning.has_value());
    EXPECT_NE(editorDecision.detailWarning->find("only active runtime backend"), std::string::npos);

    const auto gameDecision = driver.EvaluateGameMainRuntimeReadiness(
        NLS::Render::Settings::EGraphicsBackend::DX12);
    ASSERT_TRUE(gameDecision.primaryWarning.has_value());
    if (NLS::Render::Settings::IsBackendEnabledForCurrentBuild(NLS::Render::Settings::EGraphicsBackend::DX12))
        EXPECT_NE(gameDecision.primaryWarning->find("accepted phase-1 runtime startup path"), std::string::npos);
    else
        EXPECT_NE(gameDecision.primaryWarning->find("only supports DX12"), std::string::npos);
    ASSERT_TRUE(gameDecision.detailWarning.has_value());
    EXPECT_NE(gameDecision.detailWarning->find("only active runtime backend"), std::string::npos);
}

TEST(DriverNullDeviceReadinessTests, DX12NullDeviceRejectsHarnessPublishSurfaces)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.framesInFlight = 1;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, nullptr);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, nullptr);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 77u;
    snapshot.visibleOpaqueDrawCount = 1u;

    NLS::Render::Context::RenderScenePackage renderScenePackage;
    renderScenePackage.frameId = 77u;
    renderScenePackage.visibleDrawCount = 1u;
    renderScenePackage.hasVisibleDraws = true;

    EXPECT_FALSE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(
        driver,
        snapshot));
    EXPECT_FALSE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        renderScenePackage));

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->GetInFlightDepth(), 0u);
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
