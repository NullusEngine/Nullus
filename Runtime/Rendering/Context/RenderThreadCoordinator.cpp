#include <chrono>

#include <Debug/Logger.h>

#include "Profiling/Profiler.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/RhiThreadCoordinator.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace NLS::Render::Context
{
    namespace
    {
        bool ShouldBlockThreadedSnapshotPublishForPendingResize(
            const DriverImpl& impl,
            const FrameSnapshot& snapshot)
        {
            return impl.threadedLifecycle != nullptr &&
                impl.hasPendingSwapchainResize &&
                snapshot.targetsSwapchain;
        }

        bool CanPublishThreadedRenderWork(const DriverImpl& impl)
        {
            return !impl.deviceLostDetected.load(std::memory_order_acquire) &&
                !impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire);
        }

        RenderScenePackage BuildEmptyRenderScenePackage(const FrameSnapshot& snapshot)
        {
            RenderScenePackage package;
            package.frameId = snapshot.frameId;
            package.targetsSwapchain = snapshot.targetsSwapchain;
            package.clearColorValue = snapshot.clearColor;
            package.clearColorBuffer = snapshot.clearColorBuffer;
            package.clearDepthBuffer = snapshot.clearDepthBuffer;
            package.clearStencilBuffer = snapshot.clearStencilBuffer;
            package.renderWidth = snapshot.renderWidth;
            package.renderHeight = snapshot.renderHeight;
            package.sceneGameObjectCount = snapshot.sceneGameObjectCount;
            package.recordedDrawCommands.clear();
            package.visibleDrawCount = 0u;
            package.opaqueDrawCount = 0u;
            package.decalDrawCount = 0u;
            package.transparentDrawCount = 0u;
            package.skyboxDrawCount = 0u;
            package.helperDrawCount = 0u;
            package.hasVisibleDraws = false;
            package.hasLightingData = false;
            package.frameDataReady = false;
            package.objectDataReady = false;
            package.lightingDataReady = false;
            package.hasOpaquePass = false;
            package.hasDecalPass = false;
            package.hasTransparentPass = false;
            package.hasSkyboxPass = false;
            package.hasHelperPass = false;
            package.passPlanCount = 0u;
            package.drawCommandCount = 0u;
            package.materialBatchCount = 0u;
            package.renderTargetUseCount = package.targetsSwapchain ? 1u : 2u;
            package.containsCommandInputs = false;
            package.passCommandInputs.clear();
            package.parallelCommandWorkUnits.clear();
            package.workUnitDependencyEdges.clear();
            package.parallelCommandWorkUnitCount = 0u;
            package.containsParallelCommandWorkUnits = false;
            package.hasAsyncComputeWorkload = false;
            package.asyncComputeWorkloadCount = 0u;
            package.containsComputeDispatchInputs = false;
            package.computeDispatchInputs.clear();
            package.extractedTextures.clear();
            return package;
        }

        struct ThreadedPublicationSlotReservation
        {
            std::optional<size_t> slotIndex;
            bool ownedByPublication = false;
        };

        ThreadedPublicationSlotReservation ReserveThreadedPublicationSlot(Driver& driver)
        {
            ThreadedPublicationSlotReservation reservation;
            reservation.slotIndex = DriverRendererAccess::GetReservedFrameContextSlotIndex(driver);
            if (reservation.slotIndex.has_value())
                return reservation;

            if (!DriverRendererAccess::HasExplicitRHI(driver))
                return reservation;

            reservation.slotIndex = DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(driver);
            reservation.ownedByPublication = reservation.slotIndex.has_value();
            return reservation;
        }

        void ReleaseOwnedThreadedPublicationSlot(
            Driver& driver,
            const ThreadedPublicationSlotReservation& reservation)
        {
            if (!reservation.ownedByPublication || !reservation.slotIndex.has_value())
                return;

            (void)DriverRendererAccess::ReleaseReservedFrameContextSlotIndex(driver, reservation.slotIndex.value());
        }
    }

    bool RenderThreadCoordinator::IsThreadedRenderingEnabled(const Driver& driver)
    {
        return driver.IsThreadedRenderingEnabled();
    }

    RenderScenePreparingResolutionDesc Detail::BuildRenderScenePreparingResolutionDesc()
    {
        RenderScenePreparingResolutionDesc desc;
        desc.buildSnapshotHarnessRenderScenePackage = [](const FrameSnapshot& harnessSnapshot)
        {
            NLS_LOG_WARNING(
                "RenderThreadCoordinator::ResolveRenderScenePreparing: snapshot-harness scene-package construction was requested on the DX12-aligned mainline; resolving to an empty render-scene package.");
            return BuildEmptyRenderScenePackage(harnessSnapshot);
        };
        desc.buildPreparedBuilderMissingRenderScenePackage = [](const FrameSnapshot& missingBuilderSnapshot)
        {
            NLS_LOG_WARNING(
                "RenderThreadCoordinator::ResolveRenderScenePreparing: prepared frame reached render-scene completion without a prepared render-scene builder; resolving to an empty prepared-builder-missing package.");
            return BuildEmptyRenderScenePackage(missingBuilderSnapshot);
        };
        return desc;
    }

    bool RenderThreadCoordinator::BeginRendererFrame(Driver& driver, const bool acquireSwapchainImage)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->explicitDevice == nullptr || IsThreadedRenderingEnabled(driver))
            return true;

        return RhiThreadCoordinator::BeginStandaloneExplicitFrame(driver, acquireSwapchainImage);
    }

    void RenderThreadCoordinator::EndRendererFrame(Driver& driver, const bool presentSwapchain)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->explicitDevice == nullptr || IsThreadedRenderingEnabled(driver))
            return;

        RhiThreadCoordinator::EndStandaloneExplicitFrame(driver, presentSwapchain);
    }

    bool RenderThreadCoordinator::TryPublishHarnessFrameSnapshot(
        Driver& driver,
        const FrameSnapshot& snapshot,
        size_t* publishedSlotIndex)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->threadedLifecycle == nullptr)
            return false;

        if (!Detail::SupportsThreadedFoundationExecution(*driver.m_impl))
            return false;

        if (!Detail::AllowsThreadedHarnessPublish(*driver.m_impl))
            return false;

        if (!CanPublishThreadedRenderWork(*driver.m_impl))
            return false;

        if (snapshot.targetsSwapchain &&
            driver.m_impl->hasPendingSwapchainResize &&
            driver.m_impl->threadedLifecycle->GetInFlightDepth() == 0u)
        {
            driver.ApplyPendingSwapchainResize();
        }

        if (ShouldBlockThreadedSnapshotPublishForPendingResize(*driver.m_impl, snapshot))
            return false;

        auto resolvedSnapshot = snapshot;
        if (resolvedSnapshot.frameId == 0u)
            resolvedSnapshot.frameId = driver.m_impl->nextThreadedFrameId++;

        const bool published = driver.m_impl->threadedLifecycle->PublishFrameSnapshot(
            resolvedSnapshot,
            std::chrono::milliseconds(driver.m_impl->threadedPublishRetirementWaitMs),
            publishedSlotIndex);
        if (published)
            Detail::NotifyThreadedWorkers(*driver.m_impl);
        return published;
    }

    bool RenderThreadCoordinator::TryPublishHarnessPreparedFrame(
        Driver& driver,
        const FrameSnapshot& snapshot,
        const RenderScenePackage& renderScenePackage,
        size_t* publishedSlotIndex)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->threadedLifecycle == nullptr)
            return false;

        if (!Detail::SupportsThreadedFoundationExecution(*driver.m_impl))
            return false;

        if (!Detail::AllowsThreadedHarnessPublish(*driver.m_impl))
            return false;

        if (!CanPublishThreadedRenderWork(*driver.m_impl))
            return false;

        if (snapshot.targetsSwapchain &&
            driver.m_impl->hasPendingSwapchainResize &&
            driver.m_impl->threadedLifecycle->GetInFlightDepth() == 0u)
        {
            driver.ApplyPendingSwapchainResize();
        }

        if (ShouldBlockThreadedSnapshotPublishForPendingResize(*driver.m_impl, snapshot))
            return false;

        auto resolvedSnapshot = snapshot;
        if (resolvedSnapshot.frameId == 0u)
            resolvedSnapshot.frameId = driver.m_impl->nextThreadedFrameId++;

        auto resolvedRenderScenePackage = renderScenePackage;
        resolvedRenderScenePackage.frameId = resolvedSnapshot.frameId;

        const auto slotReservation = ReserveThreadedPublicationSlot(driver);
        if (DriverRendererAccess::HasExplicitRHI(driver) && !slotReservation.slotIndex.has_value())
            return false;

        if (!CanPublishThreadedRenderWork(*driver.m_impl))
        {
            ReleaseOwnedThreadedPublicationSlot(driver, slotReservation);
            return false;
        }

        const bool published = driver.m_impl->threadedLifecycle->TryPublishPreparedFrame(
            resolvedSnapshot,
            resolvedRenderScenePackage,
            publishedSlotIndex);
        if (!published)
            ReleaseOwnedThreadedPublicationSlot(driver, slotReservation);
        if (published)
            Detail::NotifyThreadedWorkers(*driver.m_impl);
        return published;
    }

    bool RenderThreadCoordinator::TryPublishPreparedFrameBuilder(
        Driver& driver,
        const FrameSnapshot& snapshot,
        PreparedRenderSceneBuilder renderSceneBuilder,
        size_t* publishedSlotIndex,
        uint64_t* publishedFrameId)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->threadedLifecycle == nullptr)
            return false;

        if (!Detail::SupportsThreadedFoundationExecution(*driver.m_impl))
            return false;

        if (!CanPublishThreadedRenderWork(*driver.m_impl))
        {
            if (publishedFrameId != nullptr)
                *publishedFrameId = 0u;
            return false;
        }

        if (snapshot.targetsSwapchain &&
            driver.m_impl->hasPendingSwapchainResize &&
            driver.m_impl->threadedLifecycle->GetInFlightDepth() == 0u)
        {
            driver.ApplyPendingSwapchainResize();
        }

        if (ShouldBlockThreadedSnapshotPublishForPendingResize(*driver.m_impl, snapshot))
            return false;

        auto resolvedSnapshot = snapshot;
        if (resolvedSnapshot.frameId == 0u)
            resolvedSnapshot.frameId = driver.m_impl->nextThreadedFrameId++;

        auto resolvedRenderSceneBuilder =
            [renderSceneBuilder = std::move(renderSceneBuilder), frameId = resolvedSnapshot.frameId]() mutable
        {
            auto renderScenePackage = renderSceneBuilder();
            renderScenePackage.frameId = frameId;
            return renderScenePackage;
        };

        const auto slotReservation = ReserveThreadedPublicationSlot(driver);
        if (DriverRendererAccess::HasExplicitRHI(driver) && !slotReservation.slotIndex.has_value())
        {
            if (publishedFrameId != nullptr)
                *publishedFrameId = 0u;
            return false;
        }

        if (!CanPublishThreadedRenderWork(*driver.m_impl))
        {
            ReleaseOwnedThreadedPublicationSlot(driver, slotReservation);
            if (publishedFrameId != nullptr)
                *publishedFrameId = 0u;
            return false;
        }

        const bool published = driver.m_impl->threadedLifecycle->PublishPreparedFrameBuilder(
            resolvedSnapshot,
            std::move(resolvedRenderSceneBuilder),
            std::chrono::milliseconds(driver.m_impl->threadedPublishRetirementWaitMs),
            publishedSlotIndex);
        if (!published)
            ReleaseOwnedThreadedPublicationSlot(driver, slotReservation);
        if (publishedFrameId != nullptr)
            *publishedFrameId = published ? resolvedSnapshot.frameId : 0u;
        if (published)
            Detail::NotifyThreadedWorkers(*driver.m_impl);
        return published;
    }

    bool RenderThreadCoordinator::DrainPendingRenderFrameBuildsSynchronously(Driver& driver)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->threadedLifecycle == nullptr)
            return false;

        bool progressed = false;
        size_t slotIndex = 0u;
        while (driver.m_impl->threadedLifecycle->TryBeginNextRenderFrameBuild(
            &slotIndex,
            nullptr))
        {
            const auto resolutionDesc = Detail::BuildRenderScenePreparingResolutionDesc();
            {
                NLS_PROFILE_NAMED_SCOPE("RenderThreadCoordinator::ResolveRenderScenePreparing");
                if (driver.m_impl->threadedLifecycle->ResolveRenderScenePreparing(
                    slotIndex,
                    resolutionDesc))
                {
                    NLS_PROFILE_NAMED_SCOPE("RenderThreadCoordinator::NotifyThreadedWorkers");
                    Detail::NotifyThreadedWorkers(*driver.m_impl);
                }
            }
            progressed = true;
        }

        return progressed;
    }

    ThreadedFrameTelemetry RenderThreadCoordinator::GetThreadedFrameTelemetry(const Driver& driver)
    {
        if (driver.m_impl->threadedLifecycle == nullptr)
            return {};

        return driver.m_impl->threadedLifecycle->GetTelemetry();
    }

    std::optional<ThreadedFrameTelemetry> RenderThreadCoordinator::TryGetThreadedFrameTelemetry(const Driver& driver)
    {
        if (driver.m_impl->threadedLifecycle == nullptr)
            return std::nullopt;

        return driver.m_impl->threadedLifecycle->TryGetTelemetry();
    }
}
