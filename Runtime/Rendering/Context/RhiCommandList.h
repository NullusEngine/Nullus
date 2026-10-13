#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "RenderDef.h"
#include "Rendering/Context/RenderCommandPipe.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Core/RHISync.h"

namespace NLS::Render::Context
{
    /**
     * Stage of one deferred RHI command list as it travels the pipeline.
     * Mirrors UE's dispatch / translate / submit split in FRHICommandListExecutor.
     */
    enum class RhiPipelineStage : uint8_t
    {
        Dispatch = 0u,
        Translate,
        Submit,
        Gpu
    };

    /** Shared state of one pipeline-stage signal. */
    struct RhiStageEventState final
    {
        mutable std::mutex mutex;
        std::condition_variable condition;
        bool complete = false;
        bool success = false;
        std::string diagnostic;
        uint64_t sequence = 0u;
    };

    /**
     * Copyable completion signal for one pipeline stage of one command list.
     *
     * The four public aliases below share this implementation but stay distinct
     * types, so an API can require a translate event where a submit event would be
     * meaningless while failure and diagnostic handling stays in one place. The
     * implementation is header-only so the template crosses the module boundary
     * without relying on explicit instantiation export.
     */
    template <RhiPipelineStage Stage>
    class RhiStageEvent final
    {
    public:
        RhiStageEvent() = default;

        static constexpr RhiPipelineStage kStage = Stage;

        // The notification must only wake the owner. It runs before waiters can
        // observe completion, so teardown cannot destroy its target mid-call.
        void Signal(const bool success = true, std::string diagnostic = {},
            const std::function<void()>& notification = {})
        {
            if (m_state == nullptr)
                return;
            {
                std::lock_guard lock(m_state->mutex);
                // First signal wins: a later stage failure must not overwrite an
                // already reported outcome for this stage.
                if (!m_state->complete)
                {
                    m_state->complete = true;
                    m_state->success = success;
                    m_state->diagnostic = std::move(diagnostic);
                    if (notification)
                        notification();
                }
            }
            m_state->condition.notify_all();
        }

        void Wait() const
        {
            if (m_state == nullptr)
                return;
            std::unique_lock lock(m_state->mutex);
            m_state->condition.wait(lock, [this] { return m_state->complete; });
        }

        [[nodiscard]] bool WaitFor(const std::chrono::milliseconds timeout) const
        {
            if (m_state == nullptr)
                return true;
            std::unique_lock lock(m_state->mutex);
            return m_state->condition.wait_for(
                lock,
                timeout,
                [this] { return m_state->complete; });
        }

        [[nodiscard]] bool IsValid() const noexcept { return m_state != nullptr; }

        [[nodiscard]] bool IsComplete() const
        {
            if (m_state == nullptr)
                return true;
            std::lock_guard lock(m_state->mutex);
            return m_state->complete;
        }

        [[nodiscard]] bool Succeeded() const
        {
            if (m_state == nullptr)
                return false;
            std::lock_guard lock(m_state->mutex);
            return m_state->complete && m_state->success;
        }

        [[nodiscard]] std::string GetDiagnostic() const
        {
            if (m_state == nullptr)
                return {};
            std::lock_guard lock(m_state->mutex);
            return m_state->diagnostic;
        }

        [[nodiscard]] uint64_t GetSequence() const
        {
            if (m_state == nullptr)
                return 0u;
            std::lock_guard lock(m_state->mutex);
            return m_state->sequence;
        }

        void SetSequence(const uint64_t sequence)
        {
            if (m_state == nullptr)
                return;
            std::lock_guard lock(m_state->mutex);
            m_state->sequence = sequence;
        }

    private:
        std::shared_ptr<RhiStageEventState> m_state { std::make_shared<RhiStageEventState>() };
    };

    using RhiDispatchEvent = RhiStageEvent<RhiPipelineStage::Dispatch>;
    using RhiTranslateEvent = RhiStageEvent<RhiPipelineStage::Translate>;
    using RhiSubmitEvent = RhiStageEvent<RhiPipelineStage::Submit>;
    using RhiGpuFence = RhiStageEvent<RhiPipelineStage::Gpu>;

    class RhiCommandList;

    /** One translated batch ready for the GPU queue. */
    struct NLS_RENDER_API RhiTranslatedBatch
    {
        uint64_t sequence = 0u;
        RHI::QueueType queueType = RHI::QueueType::Graphics;
        std::string debugName;
        std::shared_ptr<RHI::RHICommandBuffer> commandBuffer;
        std::shared_ptr<RHI::RHIFence> signalFence;
        std::vector<std::shared_ptr<RHI::RHISemaphore>> waitSemaphores;
        std::vector<std::shared_ptr<RHI::RHISemaphore>> signalSemaphores;
        bool present = false;
        bool offscreenOnly = false;
        uint64_t commandCount = 0u;
        RhiSubmitEvent submitEvent;
        RhiGpuFence gpuEvent;
        std::shared_ptr<RhiCommandList> commandListKeepAlive;
        bool gpuWorkQuarantined = false;
    };

    /**
     * Deferred backend-neutral command list. UE equivalent: FRHICommandList.
     *
     * The render thread records command bodies while holding no RHI state; the
     * executor later replays them into a real RHICommandBuffer on a worker. A list
     * is immutable once Close() has run.
     */
    class NLS_RENDER_API RhiCommandList final
        : public std::enable_shared_from_this<RhiCommandList>
    {
    public:
        using RecordFunction = std::function<void(RHI::RHICommandBuffer&)>;

        void SetDebugName(std::string debugName);
        void SetQueueType(RHI::QueueType queueType);
        void SetSignalFence(std::shared_ptr<RHI::RHIFence> fence);
        void AddWaitSemaphore(std::shared_ptr<RHI::RHISemaphore> semaphore);
        void AddSignalSemaphore(std::shared_ptr<RHI::RHISemaphore> semaphore);
        void RequestPresent(bool present);
        void MarkOffscreenOnly(bool offscreenOnly);

        /** Appends one recording step. Ignored once the list is closed. */
        bool Record(std::string debugName, RecordFunction body);

#if defined(NLS_ENABLE_TEST_HOOKS)
        /**
         * Observes recording without exposing the executor's private execution
         * state. A test that has to prove a list recorded *before* some other event
         * cannot use the native command buffer's Begin: a deferred recorder only
         * flips its own flag here and touches the native buffer during translation,
         * which happens after submission was scheduled.
         */
        using RecordObserverForTesting = std::function<void(RhiCommandList&, std::string_view)>;
        static void SetRecordObserverForTesting(RecordObserverForTesting observer);
        static void ClearRecordObserverForTesting();
#endif

        /** Convenience: records a resource-transition command. */
        bool RecordBarrier(RHI::RHIBarrierDesc barrier, std::string debugName = {});

        void Close();

        [[nodiscard]] bool IsRecording() const noexcept { return m_state == State::Recording; }
        [[nodiscard]] bool IsClosed() const noexcept { return m_state == State::Closed; }
        [[nodiscard]] bool IsSubmitted() const noexcept { return m_state == State::Submitted; }
        [[nodiscard]] size_t GetCommandCount() const noexcept { return m_commands.size(); }
        [[nodiscard]] const std::string& GetDebugName() const noexcept { return m_debugName; }
        [[nodiscard]] RHI::QueueType GetQueueType() const noexcept { return m_queueType; }
        [[nodiscard]] uint64_t GetSequence() const noexcept { return m_sequence; }
        [[nodiscard]] bool RequestsPresent() const noexcept { return m_present; }
        [[nodiscard]] bool IsOffscreenOnly() const noexcept { return m_offscreenOnly; }
        [[nodiscard]] const std::shared_ptr<RHI::RHIFence>& GetSignalFence() const noexcept
        {
            return m_signalFence;
        }
        [[nodiscard]] const std::vector<std::shared_ptr<RHI::RHISemaphore>>& GetWaitSemaphores()
            const noexcept
        {
            return m_waitSemaphores;
        }
        [[nodiscard]] const std::vector<std::shared_ptr<RHI::RHISemaphore>>& GetSignalSemaphores()
            const noexcept
        {
            return m_signalSemaphores;
        }
        /** Mutable so the executor can publish stage completions. */
        [[nodiscard]] RhiDispatchEvent& GetDispatchEvent() noexcept
        {
            return m_dispatchEvent;
        }
        [[nodiscard]] RhiTranslateEvent& GetTranslateEvent() noexcept
        {
            return m_translateEvent;
        }
        [[nodiscard]] RhiSubmitEvent& GetSubmitEvent() noexcept { return m_submitEvent; }
        [[nodiscard]] RhiGpuFence& GetGpuFence() noexcept { return m_gpuEvent; }

        /**
         * Hands the closed list to the global executor. Fails when the list is
         * still recording or when the executor rejected it.
         */
        bool Submit();

        // Executor-facing: replays the recorded bodies into a real command buffer.
        [[nodiscard]] bool Translate(
            RHI::RHICommandBuffer& commandBuffer,
            std::string* diagnostic = nullptr) const;
        [[nodiscard]] bool TranslateNative(
            RHI::RHICommandBuffer& commandBuffer,
            std::string* diagnostic = nullptr) const;
        void CommitResourceStates(RHI::RHICommandBuffer& commandBuffer) const;

    private:
        enum class State : uint8_t
        {
            Recording = 0u,
            Closed,
            Submitted
        };

        struct RecordedCommand final
        {
            std::string debugName;
            RecordFunction body;
        };

        // The executor owns sequence assignment and the submit transition.
        friend class RhiCommandListExecutor;

        std::string m_debugName;
        RHI::QueueType m_queueType = RHI::QueueType::Graphics;
        std::vector<RecordedCommand> m_commands;
        std::vector<RHI::RHIBarrierDesc> m_resourceStateTransitions;
        std::shared_ptr<RHI::RHIFence> m_signalFence;
        std::vector<std::shared_ptr<RHI::RHISemaphore>> m_waitSemaphores;
        std::vector<std::shared_ptr<RHI::RHISemaphore>> m_signalSemaphores;
        bool m_present = false;
        bool m_offscreenOnly = false;
        uint64_t m_sequence = 0u;
        State m_state = State::Recording;
        RhiDispatchEvent m_dispatchEvent;
        RhiTranslateEvent m_translateEvent;
        RhiSubmitEvent m_submitEvent;
        RhiGpuFence m_gpuEvent;
    };
}
