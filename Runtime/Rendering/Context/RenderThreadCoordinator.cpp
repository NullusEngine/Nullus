#include <algorithm>
#include <chrono>
#include <stdexcept>

#include <Debug/Logger.h>

#include "Profiling/Profiler.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/Context/RenderCommandPipe.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace NLS::Render::Context
{
    namespace
    {
        struct PendingFrameCommand final
        {
            std::shared_ptr<RenderSceneBuildCommand> sceneBuilder;
            std::function<void()> cancel;
            std::function<void()> complete;
            bool executed = false;

            ~PendingFrameCommand()
            {
                if (!executed && cancel)
                    cancel();
                if (complete)
                    complete();
            }
        };

        void FailFrameReadbacks(const RenderFramePacket& packet, const std::string& diagnostic)
        {
            for (const auto& request : packet.postSubmitTextureReadbacks)
            {
                if (request.state == nullptr)
                    continue;
                std::lock_guard lock(request.state->mutex);
                if (request.state->beginAttempted)
                    continue;
                request.state->beginAttempted = true;
                request.state->beginInProgress = false;
                request.state->beginSucceeded = false;
                request.state->resultCode = RHI::RHIReadbackStatusCode::BackendFailure;
                request.state->resultMessage = diagnostic;
                request.state->completion.reset();
            }
        }

        std::string CurrentBuildFailure()
        {
            try { throw; }
            catch (const std::exception& exception) { return exception.what(); }
            catch (...) { return "non-std exception; use a native debugger for SEH diagnostics"; }
        }

        bool ShouldBlockFramePublicationForPendingResize(const DriverImpl& impl)
        {
            // Resize drains the shared allocator ring. Offscreen producers must
            // also pause admission or a busy Scene View can keep that ring occupied.
            return impl.hasPendingSwapchainResize.load(std::memory_order_acquire);
        }

        bool CanPublishThreadedRenderWork(const DriverImpl& impl)
        {
            return !HasRenderFatalError() &&
                !impl.deviceLostDetected.load(std::memory_order_acquire) &&
                !impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire);
        }

        struct ThreadedPublicationSlotReservation
        {
            std::optional<size_t> slotIndex;
            bool ownedByPublication = false;
        };

        ThreadedPublicationSlotReservation ReserveThreadedPublicationSlot(
            Driver& driver,
            const bool waitForRetirement,
            const bool targetsSwapchain)
        {
            ThreadedPublicationSlotReservation reservation;
            reservation.slotIndex = DriverRendererAccess::GetReservedFrameContextSlotIndex(driver);
            if (reservation.slotIndex.has_value())
                return reservation;

            reservation.slotIndex = DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(
                driver,
                waitForRetirement,
                targetsSwapchain);
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

        class ScopedBackgroundPreviewPublicationRequest final
        {
        public:
            ScopedBackgroundPreviewPublicationRequest(DriverImpl& impl, const bool backgroundPreview)
                : m_request(backgroundPreview ? &impl.backgroundPreviewPublicationRequested : nullptr)
            {
                if (m_request != nullptr)
                    m_request->store(true, std::memory_order_release);
            }

            ~ScopedBackgroundPreviewPublicationRequest()
            {
                if (m_request != nullptr)
                    m_request->store(false, std::memory_order_release);
            }

            ScopedBackgroundPreviewPublicationRequest(const ScopedBackgroundPreviewPublicationRequest&) = delete;
            ScopedBackgroundPreviewPublicationRequest& operator=(const ScopedBackgroundPreviewPublicationRequest&) = delete;

        private:
            std::atomic_bool* m_request = nullptr;
        };
    }

    bool RenderThreadCoordinator::IsThreadedRenderingEnabled(const Driver& driver)
    {
        return driver.IsThreadedRenderingEnabled();
    }

    void RenderThreadCoordinator::EnqueuePublishedFrame(
        Driver& driver, const size_t slotIndex, RenderFramePacket packet,
        std::shared_ptr<RenderSceneBuildCommand> renderSceneBuilder)
    {
        const std::weak_ptr<DriverImpl::RenderCommandFlushLifetime> lifetime =
            driver.m_impl->renderCommandFlushLifetime;
        uint64_t leaseGeneration = 0u;
        uint64_t latencyFrameId = 0u;
        {
            std::lock_guard retirementLock(driver.m_impl->frameResourceRetirementMutex);
            if (driver.m_impl->frameResourceSlots != nullptr)
                if (const auto* slot = driver.m_impl->frameResourceSlots->PeekSlot(slotIndex))
                    leaseGeneration = slot->leaseGeneration;
            if (const auto& lease = driver.m_impl->reservedFrameResourceLease;
                lease.has_value() && lease->slotIndex == slotIndex && lease->generation == leaseGeneration)
            {
                latencyFrameId = lease->latencyFrameId;
                driver.m_impl->reservedFrameResourceLease.reset();
            }
        }
        auto pending = std::make_shared<PendingFrameCommand>();
        pending->sceneBuilder = std::move(renderSceneBuilder);
        auto& impl = *driver.m_impl;
        ++impl.pendingRenderFrameCommands;
        if (packet.targetsSwapchain)
            ++impl.pendingSwapchainRenderCommands;
        {
            std::lock_guard lock(impl.frameTelemetryMutex);
            ++impl.frameTelemetry.publishedFrameCount;
            impl.frameTelemetry.latestPublishedFrameId = packet.frameId;
            impl.frameTelemetry.publishOrigin = ThreadedFramePublishOrigin::PreparedBuilder;
            impl.frameTelemetry.publishState = Data::FramePublishState::Open;
            impl.frameTelemetry.retirementState = Data::FrameRetirementState::Pending;
        }
        pending->complete = [lifetime, implPtr = &impl, swapchain = packet.targetsSwapchain]
        {
            if (lifetime.expired())
                return;
            --implPtr->pendingRenderFrameCommands;
            if (swapchain)
                --implPtr->pendingSwapchainRenderCommands;
            Detail::NotifyThreadedWorkers(*implPtr);
        };
        pending->cancel = [lifetime, driverPtr = &driver, slotIndex,
            leaseGeneration, latencyFrameId, packet]
        {
            if (!lifetime.expired())
            {
                auto diagnostic = GetRenderFatalError();
                if (diagnostic.empty())
                    diagnostic = "Render frame command was cancelled before execution";
                FailFrameReadbacks(packet, diagnostic);
                {
                    std::lock_guard lock(driverPtr->m_impl->frameTelemetryMutex);
                    auto& telemetry = driverPtr->m_impl->frameTelemetry;
                    ++telemetry.failedRetiredFrameCount;
                    telemetry.latestFailedRetiredFrameId =
                        (std::max)(telemetry.latestFailedRetiredFrameId, packet.frameId);
                }
                Detail::ReleaseUnsubmittedFrameResourceLease(*driverPtr->m_impl, slotIndex, leaseGeneration, latencyFrameId);
                Detail::NotifyThreadedWorkers(*driverPtr->m_impl);
            }
        };
        EnqueueRenderCommand<struct RenderFrameCommand>(
            [lifetime, driverPtr = &driver, slotIndex,
                leaseGeneration, latencyFrameId,
                packet = std::move(packet), pending = std::move(pending)]
            {
                if (lifetime.expired())
                    return;
                pending->executed = true;
                RenderScenePackage preparedPackage;
                try
                {
                    if (!RenderingThreadSystem::IsInRenderingThread())
                        throw std::logic_error("Render scene builds require the Render Thread");
                    if (pending->sceneBuilder == nullptr || !*pending->sceneBuilder)
                        throw std::logic_error("Render frame has no scene build command");
                    auto builder = std::move(*pending->sceneBuilder);
                    const auto buildStart = std::chrono::steady_clock::now();
                    preparedPackage = builder(slotIndex);
                    const auto buildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - buildStart).count();
                    if (buildMs >= 1000)
                        NLS_LOG_WARNING("Render frame scene build took " + std::to_string(buildMs) +
                            " ms frame=" + std::to_string(packet.frameId));
                    preparedPackage.frameId = packet.frameId;
#if defined(NLS_ENABLE_TEST_HOOKS)
                    if (driverPtr->m_impl->renderSceneObserver)
                        driverPtr->m_impl->renderSceneObserver(packet, preparedPackage);
#endif
                }
                catch (...)
                {
                    const auto diagnostic = "Render scene build failed slot=" + std::to_string(slotIndex) +
                        " frame=" + std::to_string(packet.frameId) + ": " + CurrentBuildFailure();
                    FailFrameReadbacks(packet, diagnostic);
                    {
                        std::lock_guard lock(driverPtr->m_impl->frameTelemetryMutex);
                        auto& telemetry = driverPtr->m_impl->frameTelemetry;
                        ++telemetry.failedRetiredFrameCount;
                        telemetry.latestFailedRetiredFrameId = packet.frameId;
                    }
                    Detail::ReleaseUnsubmittedFrameResourceLease(*driverPtr->m_impl, slotIndex, leaseGeneration, latencyFrameId);
                    throw std::runtime_error(diagnostic);
                }
                if (!RhiCommandListExecutor::DispatchRenderFrame(*driverPtr, slotIndex, std::move(preparedPackage)))
                    throw std::runtime_error("Render frame command could not dispatch its RHI work");
                Detail::NotifyThreadedWorkers(*driverPtr->m_impl);
            });
        if (latencyFrameId != 0u && driver.m_impl->frameLatency != nullptr)
        {
            RenderCommandFence fence;
            fence.BeginFence(RenderCommandSyncDepth::Rhi);
            (void)driver.m_impl->frameLatency->ArmFrameFence(latencyFrameId, std::move(fence));
        }
        Detail::NotifyThreadedWorkers(*driver.m_impl);
    }

    bool RenderThreadCoordinator::BeginRendererFrame(
        Driver& driver,
        const bool waitForRetirement,
        const bool targetsSwapchain)
    {
        NLS_PROFILE_SCOPE();
        if (IsThreadedRenderingEnabled(driver))
        {
            if (!CanPublishThreadedRenderWork(*driver.m_impl))
                return false;
            if (ShouldBlockFramePublicationForPendingResize(*driver.m_impl))
                return false;
            // A non-blocking Present can leave UI pending when scene producers fill
            // the shared budget. Give that consumer capacity before another producer.
            if (!targetsSwapchain && !driver.m_impl->hasPendingSwapchainResize &&
                driver.m_impl->explicitSwapchain != nullptr &&
                driver.m_impl->pendingUiSnapshotConsumerCommands.load(std::memory_order_acquire) == 0u &&
                !DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value())
            {
                const auto& desc = driver.m_impl->explicitSwapchain->GetDesc();
                // Opportunistic: the ordered lane publishes the same UI snapshot
                // from the present path when the ring is busy, so the game thread
                // must not block here just to get the swapchain a slot. A blocking
                // publish here measured as roughly half of the Scene View's
                // BeginFrame stall, because the tick then waited for two slots.
                (void)DriverUIAccess::PublishUiOnlyFrame(
                    driver, desc.width, desc.height, nullptr, nullptr, false);
            }
            // Reserve before scene capture, so a frame waiting for capacity does
            // not consume the next outstanding scene synchronization request.
            return ReserveThreadedPublicationSlot(driver, waitForRetirement, targetsSwapchain).slotIndex.has_value();
        }

        // The standalone inline frame path is gone: a frame is always prepared and
        // submitted through the threaded pipeline. Reaching here with a live device
        // means the driver cannot run that pipeline - either the backend does not
        // support it, or ShutdownThreadedRendering has already run. Returning a bare
        // false used to surface as an endless "render-busy" with no cause named, so
        // the reason is reported once per driver.
        if (!driver.m_impl->reportedMissingThreadedPipeline)
        {
            driver.m_impl->reportedMissingThreadedPipeline = true;
            NLS_LOG_ERROR(
                "RenderThreadCoordinator::BeginRendererFrame: refusing to start a frame "
                "because the threaded render pipeline is not available for this device "
                "(threadedWorkersRunning=" +
                std::to_string(driver.m_impl->threadedWorkersRunning ? 1u : 0u) +
                ", foundationSupported=" +
                std::to_string(
                    Detail::SupportsThreadedFoundationExecution(*driver.m_impl) ? 1u : 0u) +
                "). The inline standalone frame path was removed in favour of the single "
                "threaded pipeline.");
        }
        return false;
    }

    void RenderThreadCoordinator::EndRendererFrame(Driver& driver)
    {
        NLS_PROFILE_SCOPE();
        if (const auto slot = DriverRendererAccess::GetReservedFrameContextSlotIndex(driver); slot.has_value())
            (void)DriverRendererAccess::ReleaseReservedFrameContextSlotIndex(driver, *slot);
    }


    bool RenderThreadCoordinator::TryEnqueueRenderFrame(
        Driver& driver,
        const RenderFramePacket& snapshot,
        RenderSceneBuildCommand renderSceneBuilder,
        const bool applyPendingSwapchainResize,
        size_t* publishedSlotIndex,
        uint64_t* publishedFrameId,
        const bool backgroundPreview,
        const bool waitForRetirement)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->frameResourceSlots == nullptr)
            return false;

        if (!renderSceneBuilder)
            return false;

        const ScopedBackgroundPreviewPublicationRequest backgroundPreviewRequest(
            *driver.m_impl,
            backgroundPreview);
        if (!backgroundPreview &&
            driver.m_impl->backgroundPreviewPublicationRequested.load(std::memory_order_acquire))
        {
            if (publishedFrameId != nullptr)
                *publishedFrameId = 0u;
            return false;
        }

        if (!Detail::SupportsThreadedFoundationExecution(*driver.m_impl))
            return false;

        if (!CanPublishThreadedRenderWork(*driver.m_impl))
        {
            if (publishedFrameId != nullptr)
                *publishedFrameId = 0u;
            return false;
        }

        if (applyPendingSwapchainResize &&
            driver.m_impl->hasPendingSwapchainResize &&
            Detail::GetPendingCpuFrameCount(*driver.m_impl) == 0u)
        {
            driver.ApplyPendingSwapchainResize();
        }

        if (ShouldBlockFramePublicationForPendingResize(*driver.m_impl))
        {
            if (publishedFrameId != nullptr)
                *publishedFrameId = 0u;
            return false;
        }

        auto resolvedSnapshot = snapshot;
        if (resolvedSnapshot.frameId == 0u)
            resolvedSnapshot.frameId = driver.m_impl->nextThreadedFrameId++;

        auto resolvedRenderSceneBuilder =
            [driverPtr = &driver, renderSceneBuilder = std::move(renderSceneBuilder), frameId = resolvedSnapshot.frameId](
                const size_t slotIndex) mutable
        {
            std::optional<ScopedFrameResourceContext> frameResources;
            if (DriverRendererAccess::GetFrameContextSlotCount(*driverPtr) != 0u)
                frameResources.emplace(*driverPtr, slotIndex);
            auto renderScenePackage = renderSceneBuilder(slotIndex);
            renderScenePackage.frameId = frameId;
            return renderScenePackage;
        };

        const bool shouldWaitForRetirement = waitForRetirement && !backgroundPreview;
        const auto slotReservation = ReserveThreadedPublicationSlot(driver, shouldWaitForRetirement, snapshot.targetsSwapchain);
        if (!slotReservation.slotIndex.has_value())
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

        const size_t actualSlotIndex = *slotReservation.slotIndex;
        auto commandBuilder = std::make_shared<RenderSceneBuildCommand>(std::move(resolvedRenderSceneBuilder));
        const bool published = slotReservation.slotIndex.has_value();
        if (!published)
            ReleaseOwnedThreadedPublicationSlot(driver, slotReservation);
        if (publishedFrameId != nullptr)
            *publishedFrameId = published ? resolvedSnapshot.frameId : 0u;
        if (published)
        {
            if (publishedSlotIndex != nullptr)
                *publishedSlotIndex = actualSlotIndex;
            EnqueuePublishedFrame(driver, actualSlotIndex, resolvedSnapshot, std::move(commandBuilder));
        }
        return published;
    }


    ThreadedFrameTelemetry RenderThreadCoordinator::GetThreadedFrameTelemetry(const Driver& driver)
    {
        ThreadedFrameTelemetry telemetry;
        {
            std::lock_guard lock(driver.m_impl->frameTelemetryMutex);
            telemetry = driver.m_impl->frameTelemetry;
        }
        telemetry.inFlightFrameCount = Detail::GetPendingCpuFrameCount(*driver.m_impl);
        return telemetry;
    }

    std::optional<ThreadedFrameTelemetry> RenderThreadCoordinator::TryGetThreadedFrameTelemetry(const Driver& driver)
    {
        std::unique_lock lock(driver.m_impl->frameTelemetryMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return std::nullopt;
        auto telemetry = driver.m_impl->frameTelemetry;
        lock.unlock();
        telemetry.inFlightFrameCount = Detail::GetPendingCpuFrameCount(*driver.m_impl);
        return telemetry;
    }
}
