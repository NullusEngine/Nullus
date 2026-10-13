#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include "RenderDef.h"
#include "Rendering/Context/RenderCommandFence.h"

namespace NLS::Render::Context
{
	/**
	 * Frame-count based latency gate for the game thread.
	 *
	 * UE equivalent: the frame-latency limit that stalls the game thread once too
	 * many frames are still in flight. The budget is counted in frames rather than
	 * wall-clock time: with a deadline the frame loss depends on machine load, so a
	 * slow frame drops the next one even when a slot would have come free.
	 *
	 * Published frames retire through their command fences. A reservation that
	 * never publishes returns its own token without retiring another frame.
	 */
	class NLS_RENDER_API FrameLatencyController final
	{
	public:
		explicit FrameLatencyController(uint32_t maxFramesInFlight = 1u);

		/**
		 * Opens a frame, waiting while the budget is exhausted.
		 *
		 * Returns false when maxWait expires first, and the caller then decides
		 * whether to drop the frame. A zero maxWait never waits.
		 */
		[[nodiscard]] bool BeginFrame(
			std::chrono::nanoseconds maxWait = std::chrono::nanoseconds::zero());

			// Producer headroom remains available to frames that reserve no headroom.
			[[nodiscard]] std::optional<uint64_t> ReserveFrame(
				std::chrono::nanoseconds maxWait = std::chrono::nanoseconds::zero(),
				uint32_t reservedFrameCount = 0u);
		bool ArmFrameFence(uint64_t frameId, RenderCommandFence fence);
		bool CancelFrame(uint64_t frameId);
		uint32_t PollCompletedFrames();
		[[nodiscard]] std::string GetFailureDiagnostic() const;

#if defined(NLS_ENABLE_TEST_HOOKS)
		/** Manual retirement for fixtures that do not publish a render command. */
		bool CompleteOldestFrame();

		/**
		 * Drops surplus open frames so the budget matches an authoritative
		 * in-flight count. Returns how many frames were dropped.
		 */
		uint32_t ReconcileInFlight(uint32_t observedInFlight);
#endif

		/** Refuses new frames and releases all admission waiters. */
		void Stop();

		/** Reopens admission and drops every open frame. */
		void Reset();

		void SetMaxFramesInFlight(uint32_t maxFramesInFlight);
		[[nodiscard]] uint32_t GetMaxFramesInFlight() const;
		[[nodiscard]] uint32_t GetInFlightFrameCount() const;

		[[nodiscard]] uint64_t GetOpenedFrameCount() const;
		[[nodiscard]] uint64_t GetRetiredFrameCount() const;
		[[nodiscard]] uint64_t GetStalledFrameCount() const;
		[[nodiscard]] uint64_t GetStallTimeoutCount() const;
		[[nodiscard]] uint64_t GetTotalStallTimeNanoseconds() const;

	private:
		struct Frame final
		{
			uint64_t id = 0u;
			std::optional<RenderCommandFence> fence;
				bool usesReservedCapacity = false;
		};
		uint32_t RefreshCompletedFramesLocked();
		mutable std::mutex m_mutex;
		std::condition_variable m_condition;
		std::deque<Frame> m_openFrames;
		uint64_t m_nextFrameId = 1u;
		uint32_t m_maxFramesInFlight = 1u;
		bool m_stopped = false;
		std::string m_failureDiagnostic;
		uint64_t m_openedFrameCount = 0u;
		uint64_t m_retiredFrameCount = 0u;
		uint64_t m_stalledFrameCount = 0u;
		uint64_t m_stallTimeoutCount = 0u;
		uint64_t m_totalStallTimeNanoseconds = 0u;
	};
}
