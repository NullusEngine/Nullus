#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/Settings/RhiThreadMode.h"
#include "TaskGraph/TaskGraph.h"

namespace NLS::Render::Context
{
    // The mode is a rendering setting so DriverSettings can hold it without the
    // settings layer depending on the context layer.
    using RhiThreadMode = NLS::Render::Settings::RhiThreadMode;

    // Mode names come from the settings layer so there is exactly one spelling
    // per mode across the engine.
    using NLS::Render::Settings::ToString;

    /**
     * Work the rendering thread system drives. The owning module registers one
     * tick target so the thread bodies stay in the module that owns the device.
     * UE equivalent: RenderingThreadMain / FRHIThread::Run.
     */
    class IRenderingThreadTick
    {
    public:
        virtual ~IRenderingThreadTick() = default;

        /** Returns true when the tick performed work, false to let the system idle. */
        virtual bool TickRenderThread() = 0;
        virtual bool TickRhiLane() = 0;

        /**
         * Lets the owner release resources pinned by its producer lane before the
         * thread leaves. Called after the final drain and before the thread
         * unregisters from the TaskGraph.
         */
        virtual void OnRenderThreadStopping() {}
        virtual void OnRhiLaneStopping() {}
    };

    struct RenderingThreadInitParams
    {
        RhiThreadMode rhiThreadMode = RhiThreadMode::DedicatedThread;
        IRenderingThreadTick* tick = nullptr;
        // Null selects the process-lifetime global TaskGraph.
        NLS::Base::TaskGraph::TaskGraph* taskGraph = nullptr;
        bool registerGameThread = true;
        std::chrono::milliseconds idleWait { 2 };
        std::string renderThreadName = "Render Thread";
        std::string rhiThreadName = "RHI Thread";
    };

    /**
     * Unrecoverable rendering failure sink.
     *
     * A fatal rendering error is process-wide: every rendering thread loop stops
     * and every blocked waiter is released, regardless of which object detected
     * it. The render command pipe reports into this sink without needing an owner.
     */
    NLS_RENDER_API void ReportRenderFatalError(std::string diagnostic);
    NLS_RENDER_API std::string GetRenderFatalError();
    NLS_RENDER_API bool HasRenderFatalError();
#if defined(NLS_ENABLE_TEST_HOOKS)
    /** Clears the process-wide fatal error so one scenario cannot poison the next. */
    NLS_RENDER_API void ResetRenderFatalErrorForTesting();
    /** Runs isolated tests on a joined render lane, without starting a device. */
    class NLS_RENDER_API ScopedRenderThreadForTesting final
    {
    public:
        ScopedRenderThreadForTesting();
        ~ScopedRenderThreadForTesting();
        ScopedRenderThreadForTesting(const ScopedRenderThreadForTesting&) = delete;
        ScopedRenderThreadForTesting& operator=(const ScopedRenderThreadForTesting&) = delete;
    };
#endif

    class NLS_RENDER_API ScopedGameThread final
    {
    public:
        // Null registers startup identity without starting workers. Driver later
        // attaches its TaskGraph using the resolved RHI execution budget.
        explicit ScopedGameThread(NLS::Base::TaskGraph::TaskGraph* graph = nullptr);
        ~ScopedGameThread();

        ScopedGameThread(const ScopedGameThread&) = delete;
        ScopedGameThread& operator=(const ScopedGameThread&) = delete;

    private:
        NLS::Base::TaskGraph::TaskGraph* m_graph;
    };

    /**
     * Owns the render thread (always dedicated) and the logical RHI lane
     * (dedicated / TaskGraph tasks / inline).
     *
     * One instance per driver: a leaked or concurrently alive driver must not
     * inherit or tear down another driver's threads. Thread identity queries are
     * static because they describe the calling thread, not a particular owner.
     */
    class NLS_RENDER_API RenderingThreadSystem final
    {
    public:
        RenderingThreadSystem();
        ~RenderingThreadSystem();

        RenderingThreadSystem(const RenderingThreadSystem&) = delete;
        RenderingThreadSystem& operator=(const RenderingThreadSystem&) = delete;

        bool InitRenderingThreads(const RenderingThreadInitParams& params);
        void ShutdownRenderingThreads();
        [[nodiscard]] bool IsInitialized() const;

        /**
         * Stops the thread bodies without giving up the stored configuration, so
         * ResumeRenderingThreads can recreate them. Used by the editor's pause /
         * synchronous-drain paths and by tests.
         */
        void SuspendRenderingThreads();
        bool ResumeRenderingThreads();
        [[nodiscard]] bool IsSuspended() const;

        [[nodiscard]] RhiThreadMode GetRhiThreadMode() const;

        /** True once shutdown has been requested; thread loops must then drain and exit. */
        [[nodiscard]] bool IsStopRequested() const;
        void RequestStop();

        /**
         * Records an unrecoverable rendering error. Every thread loop observes it
         * and exits so a caller blocked on rendering work cannot hang.
         */
        void MarkFatalError(std::string diagnostic);
        [[nodiscard]] std::string GetFatalError() const;
        [[nodiscard]] bool HasFatalError() const;

        ///////////////////////////////////////////////////////////////////////
        // Thread identity
        ///////////////////////////////////////////////////////////////////////

        [[nodiscard]] static bool IsInGameThread();
        /** Pumps only the calling game thread's attached TaskGraph queues. */
        static size_t PumpGameThreadTasks(size_t maxTasks = 1u);
        [[nodiscard]] static bool IsInRenderingThread();
        /** Includes startup and shutdown so the game thread never takes over a live pipe. */
        [[nodiscard]] static bool HasActiveRenderThread();
        [[nodiscard]] static bool IsInRhiThread();
        [[nodiscard]] static NLS::Base::TaskGraph::NamedThread GetCurrentNamedThread();

#if defined(NLS_ENABLE_TEST_HOOKS)
        /**
         * Number of live render-thread registrations. Tests assert the fixture
         * lifecycle with it: a process that keeps a count without a thread makes
         * every later FlushRenderingCommands wait for a drain nobody can run.
         */
        [[nodiscard]] static uint32_t GetActiveRenderThreadCountForTesting();
#endif

        /**
         * Marks the calling thread as executing inside the logical RHI lane.
         * Tasks mode has no dedicated RHI thread, so IsInRhiThread() is defined by
         * this scope rather than by an OS thread id.
         */
        class NLS_RENDER_API ScopedRhiLaneScope final
        {
        public:
            ScopedRhiLaneScope();
            ~ScopedRhiLaneScope();

            ScopedRhiLaneScope(const ScopedRhiLaneScope&) = delete;
            ScopedRhiLaneScope& operator=(const ScopedRhiLaneScope&) = delete;
        };

    private:
        struct State;
        std::shared_ptr<State> m_state;
    };
}
