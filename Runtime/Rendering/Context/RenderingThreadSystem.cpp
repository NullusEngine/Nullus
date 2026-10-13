#include "Rendering/Context/RenderingThreadSystem.h"

#include <atomic>
#include <algorithm>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <Debug/Logger.h>

#include "Profiling/Profiler.h"
#include "Rendering/Context/RenderCommandPipe.h"
#include "TaskGraph/TaskGraphSystem.h"

namespace NLS::Render::Context
{
    namespace
    {
        using NLS::Base::TaskGraph::NamedThread;
        using NLS::Base::TaskGraph::TaskGraph;

        ///////////////////////////////////////////////////////////////////////
        // Thread identity
        //
        // These are thread-local / process-global rather than per-owner: they
        // describe the calling thread, and several drivers may have lived during
        // process startup and shutdown.
        ///////////////////////////////////////////////////////////////////////

        thread_local bool g_inRenderThread = false;
#if defined(NLS_ENABLE_TEST_HOOKS)
        thread_local uint32_t g_renderTestScopeDepth = 0u;
#endif
        thread_local bool g_inDedicatedRhiThread = false;
        thread_local uint32_t g_rhiLaneDepth = 0u;

        /**
         * Process-lifetime thread registries.
         *
         * These are read on every render/RHI thread iteration, and drivers that
         * outlive main() are still running those loops while other translation
         * units destroy their statics. A destroyed mutex here would turn an
         * ordinary exit into a hang, so the state is deliberately never freed.
         */
        struct ThreadRegistryState final
        {
            struct GameGraphRegistration final
            {
                TaskGraph* graph = nullptr;
                uint32_t count = 0u;
                bool ownsAttachment = false;
            };
            std::mutex gameThreadMutex;
            std::thread::id gameThreadId {};
            uint32_t gameThreadRegistrationCount = 0u;
            std::vector<GameGraphRegistration> gameGraphs;
            std::atomic<uint32_t> activeRenderThreadCount { 0u };
            std::mutex fatalErrorMutex;
            std::string fatalError;
        };

        ThreadRegistryState& RegistryState()
        {
            static ThreadRegistryState* state = new ThreadRegistryState();
            return *state;
        }

        bool RegisterGameThread(TaskGraph* graph)
        {
            ThreadRegistryState& state = RegistryState();
            std::lock_guard lock(state.gameThreadMutex);
            const auto currentId = std::this_thread::get_id();
            if (state.gameThreadRegistrationCount != 0u && state.gameThreadId != currentId)
                return false;
            if (graph != nullptr)
            {
                const auto currentLane = graph->GetCurrentNamedThread();
                if (graph->IsWorkerThread() ||
                    (currentLane != NamedThread::Any && currentLane != NamedThread::Game))
                    return false;
                auto entry = std::find_if(state.gameGraphs.begin(), state.gameGraphs.end(),
                    [graph](const auto& item) { return item.graph == graph; });
                if (entry == state.gameGraphs.end())
                {
                    const bool ownsAttachment = currentLane != NamedThread::Game;
                    if (ownsAttachment && !graph->RegisterCurrentThread(NamedThread::Game))
                        return false;
                    state.gameGraphs.push_back({ graph, 1u, ownsAttachment });
                }
                else
                    ++entry->count;
            }
            if (state.gameThreadRegistrationCount == 0u)
            {
                state.gameThreadId = currentId;
                NLS_PROFILE_REGISTER_THREAD("Game Thread");
            }
            ++state.gameThreadRegistrationCount;
            return true;
        }

        void UnregisterGameThread(TaskGraph* graph)
        {
            ThreadRegistryState& state = RegistryState();
            std::lock_guard lock(state.gameThreadMutex);
            if (state.gameThreadRegistrationCount == 0u)
                return;
            if (graph != nullptr)
            {
                const auto entry = std::find_if(state.gameGraphs.begin(), state.gameGraphs.end(),
                    [graph](const auto& item) { return item.graph == graph; });
                if (entry == state.gameGraphs.end())
                    return;
                if (--entry->count == 0u)
                {
                    if (entry->ownsAttachment)
                        graph->UnregisterThread(NamedThread::Game, state.gameThreadId);
                    state.gameGraphs.erase(entry);
                }
            }
            --state.gameThreadRegistrationCount;
            if (state.gameThreadRegistrationCount == 0u)
                state.gameThreadId = std::thread::id {};
        }
    }

    ScopedGameThread::ScopedGameThread(TaskGraph* graph)
        : m_graph(graph)
    {
        if (!RegisterGameThread(graph))
            throw std::runtime_error("Game thread registration conflicts with the current execution context");
    }

    ScopedGameThread::~ScopedGameThread()
    {
        UnregisterGameThread(m_graph);
    }

    struct RenderingThreadSystem::State final
    {
        mutable std::mutex mutex;
        std::thread renderThread;
        std::thread rhiThread;
        std::atomic_bool renderThreadExited { true };
        std::atomic_bool rhiThreadExited { true };
        RhiThreadMode rhiThreadMode = RhiThreadMode::DedicatedThread;
        IRenderingThreadTick* tick = nullptr;
        TaskGraph* taskGraph = nullptr;
        std::chrono::milliseconds idleWait { 2 };
        std::string renderThreadName;
        std::string rhiThreadName;
        bool registerGameThread = true;
        /**
         * True only while this system holds a game-thread registration, so
         * shutdown releases exactly one even when the threads never started.
         */
        bool gameThreadRegistered = false;
        bool renderThreadCounted = false;
        bool initialized = false;
        bool suspended = false;
        bool stopRequested = false;
        std::string fatalError;

        /**
         * Diagnostic phase counters.
         *
         * Written with relaxed atomics only: a probe that takes a lock can itself
         * become the reason a thread will not exit, which makes the measurement
         * worthless. A detached watcher thread reads these and writes them with raw
         * stdio so no engine lock is involved on either side.
         */
        [[nodiscard]] bool ShouldStop()
        {
            std::lock_guard lock(mutex);
            // A fatal error reported by any part of the pipeline (or by another
            // owner) stops this loop as well.
            return stopRequested || !fatalError.empty() || HasRenderFatalError();
        }

        [[nodiscard]] TaskGraph& Graph() const
        {
            return taskGraph != nullptr
                ? *taskGraph
                : NLS::Base::TaskGraph::GetGlobalTaskGraph();
        }

        template <typename Body>
        static bool RunThreadWork(const char* phase, Body&& body)
        {
            try
            {
                return body();
            }
            catch (const std::exception& error)
            {
                ReportRenderFatalError(std::string(phase) + ": " + error.what());
            }
            catch (...)
            {
                ReportRenderFatalError(std::string(phase) + ": unknown exception");
            }
            return false;
        }

        /**
         * Runs one iteration. TaskGraph work is pumped first so the tick observes
         * the same state the driver's original loop did: named-thread tasks have
         * already run when the frame build starts.
         */
        bool RunRenderIteration()
        {
            return RunThreadWork("Render thread tick", [this]
            {
                bool didWork = Graph().PumpCurrentThread() > 0u;
                IRenderingThreadTick* const owner = tick;
                if (owner != nullptr && owner->TickRenderThread())
                    didWork = true;
                return didWork;
            });
        }

        bool RunRhiIteration()
        {
            return RunThreadWork("RHI thread tick", [this]
            {
                bool didWork = Graph().PumpCurrentThread() > 0u;
                IRenderingThreadTick* const owner = tick;
                if (owner != nullptr && owner->TickRhiLane())
                    didWork = true;
                return didWork;
            });
        }

        /**
         * Sleeps in small slices so a stop request or fatal error is observed
         * promptly without needing a dedicated wake event.
         */
        void IdleWait()
        {
            const auto slice =
                idleWait.count() > 0 ? idleWait : std::chrono::milliseconds(2);
            const auto deadline = std::chrono::steady_clock::now() + slice;
            while (!ShouldStop() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        void RenderThreadMain()
        {
            g_inRenderThread = true;
            NLS_PROFILE_REGISTER_THREAD(renderThreadName);
            Graph().RegisterCurrentThread(NamedThread::Render);

            while (!ShouldStop())
            {
                if (!RunRenderIteration())
                    IdleWait();
            }

            // Drain once more so commands enqueued before the stop request are not
            // abandoned while the device is still alive.
            (void)RunRenderIteration();
            if (tick != nullptr)
                (void)RunThreadWork("Render thread shutdown", [this]
                {
                    tick->OnRenderThreadStopping();
                    return true;
                });

            Graph().UnregisterCurrentThread(NamedThread::Render);
            g_inRenderThread = false;
            renderThreadExited.store(true, std::memory_order_release);
        }

        void RhiThreadMain()
        {
            g_inDedicatedRhiThread = true;
            NLS_PROFILE_REGISTER_THREAD(rhiThreadName);
            Graph().RegisterCurrentThread(NamedThread::Rhi);

            while (!ShouldStop())
            {
                if (!RunRhiIteration())
                    IdleWait();
            }

            (void)RunRhiIteration();
            if (tick != nullptr)
                (void)RunThreadWork("RHI thread shutdown", [this]
                {
                    tick->OnRhiLaneStopping();
                    return true;
                });

            Graph().UnregisterCurrentThread(NamedThread::Rhi);
            g_inDedicatedRhiThread = false;
            rhiThreadExited.store(true, std::memory_order_release);
        }

        bool StartThreadsLocked()
        {
            try
            {
                renderThreadExited.store(false, std::memory_order_release);
                renderThread = std::thread([this] { RenderThreadMain(); });
                RegistryState().activeRenderThreadCount.fetch_add(1u, std::memory_order_release);
                renderThreadCounted = true;
                if (rhiThreadMode == RhiThreadMode::DedicatedThread)
                {
                    rhiThreadExited.store(false, std::memory_order_release);
                    rhiThread = std::thread([this] { RhiThreadMain(); });
                }
            }
            catch (...)
            {
                stopRequested = true;
                NLS_LOG_ERROR("RenderingThreadSystem: failed to create rendering threads");
                return false;
            }
            return true;
        }

        static void JoinThread(std::thread& thread, const std::atomic_bool& exited)
        {
            if (!thread.joinable())
                return;
            while (!exited.load(std::memory_order_acquire))
            {
                (void)RenderingThreadSystem::PumpGameThreadTasks();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            thread.join();
        }
    };

    RenderingThreadSystem::ScopedRhiLaneScope::ScopedRhiLaneScope()
    {
        ++g_rhiLaneDepth;
    }

    RenderingThreadSystem::ScopedRhiLaneScope::~ScopedRhiLaneScope()
    {
        if (g_rhiLaneDepth > 0u)
            --g_rhiLaneDepth;
    }

    RenderingThreadSystem::RenderingThreadSystem()
        : m_state(std::make_shared<State>())
    {
    }

    RenderingThreadSystem::~RenderingThreadSystem()
    {
        ShutdownRenderingThreads();
    }

    bool RenderingThreadSystem::InitRenderingThreads(const RenderingThreadInitParams& params)
    {
        State& state = *m_state;
        std::unique_lock lock(state.mutex);
        if (state.initialized)
        {
            // Idempotent for the same owner and tick.
            return state.tick == params.tick;
        }

        state.rhiThreadMode = params.rhiThreadMode;
        state.tick = params.tick;
        state.taskGraph = params.taskGraph;
        state.idleWait = params.idleWait;
        state.renderThreadName =
            params.renderThreadName.empty() ? std::string("Render Thread") : params.renderThreadName;
        state.rhiThreadName =
            params.rhiThreadName.empty() ? std::string("RHI Thread") : params.rhiThreadName;
        state.registerGameThread = params.registerGameThread;
        state.stopRequested = false;
        state.fatalError.clear();
        state.suspended = false;

        if (params.registerGameThread)
        {
            if (!RegisterGameThread(&state.Graph()))
            {
                NLS_LOG_ERROR("RenderingThreadSystem: game thread attachment conflicts with the current execution context");
                return false;
            }
            state.gameThreadRegistered = true;
        }

        // The render thread is always dedicated; only the RHI lane varies by mode.
        if (!state.StartThreadsLocked())
        {
            lock.unlock();
            ShutdownRenderingThreads();
            return false;
        }

        state.initialized = true;
        NLS_LOG_INFO(
            std::string("RenderingThreadSystem initialized with RHI mode ") +
            ToString(params.rhiThreadMode));
        return true;
    }

    void RenderingThreadSystem::ShutdownRenderingThreads()
    {
        if (m_state == nullptr)
            return;
        State& state = *m_state;

        std::thread renderThread;
        std::thread rhiThread;
        bool releaseGameThread = false;
        bool releaseRenderThreadCount = false;
        {
            std::lock_guard lock(state.mutex);
            // Deliberately no early return on !initialized. A thread that was
            // created before the rest of the start sequence failed is still
            // running while `initialized` is false, and skipping the join here
            // would let it outlive both this system and the objects its tick
            // reads. Taking whatever is stored and joining it is always correct:
            // with nothing stored the joins below are no-ops.
            state.stopRequested = true;
            renderThread = std::move(state.renderThread);
            rhiThread = std::move(state.rhiThread);
            releaseGameThread = state.gameThreadRegistered;
            state.gameThreadRegistered = false;
            releaseRenderThreadCount = state.renderThreadCounted;
            state.renderThreadCounted = false;
        }

        // Joining outside the lock lets the thread bodies take it while draining.
        State::JoinThread(renderThread, state.renderThreadExited);
        State::JoinThread(rhiThread, state.rhiThreadExited);

        if (releaseRenderThreadCount)
            RegistryState().activeRenderThreadCount.fetch_sub(1u, std::memory_order_release);
        if (releaseGameThread)
            UnregisterGameThread(&state.Graph());

        std::lock_guard lock(state.mutex);
        state.initialized = false;
        state.suspended = false;
        state.tick = nullptr;
        state.rhiThreadMode = RhiThreadMode::DedicatedThread;
        state.stopRequested = false;
    }

    bool RenderingThreadSystem::IsInitialized() const
    {
        std::lock_guard lock(m_state->mutex);
        return m_state->initialized;
    }

    void RenderingThreadSystem::SuspendRenderingThreads()
    {
        State& state = *m_state;

        std::thread renderThread;
        std::thread rhiThread;
        {
            std::lock_guard lock(state.mutex);
            // Same reasoning as shutdown: join whatever is stored even if the
            // start sequence never completed, or a half-started system leaks a
            // live thread into the next resume.
            const bool hasThreads =
                state.renderThread.joinable() || state.rhiThread.joinable();
            if (!hasThreads && (!state.initialized || state.suspended))
                return;

            state.stopRequested = true;
            renderThread = std::move(state.renderThread);
            rhiThread = std::move(state.rhiThread);
        }

        State::JoinThread(renderThread, state.renderThreadExited);
        State::JoinThread(rhiThread, state.rhiThreadExited);

        std::lock_guard lock(state.mutex);
        // The configuration is deliberately retained so the threads can be
        // recreated with the same tick and mode.
        state.suspended = true;
        if (state.renderThreadCounted)
        {
            RegistryState().activeRenderThreadCount.fetch_sub(1u, std::memory_order_release);
            state.renderThreadCounted = false;
        }
        state.stopRequested = false;
    }

    bool RenderingThreadSystem::ResumeRenderingThreads()
    {
        State& state = *m_state;
        std::lock_guard lock(state.mutex);
        if (!state.initialized || !state.suspended)
            return false;

        if (!state.StartThreadsLocked())
            return false;

        state.suspended = false;
        return true;
    }

    bool RenderingThreadSystem::IsSuspended() const
    {
        std::lock_guard lock(m_state->mutex);
        return m_state->suspended;
    }

    RhiThreadMode RenderingThreadSystem::GetRhiThreadMode() const
    {
        std::lock_guard lock(m_state->mutex);
        return m_state->rhiThreadMode;
    }

    bool RenderingThreadSystem::IsStopRequested() const
    {
        {
            std::lock_guard lock(m_state->mutex);
            if (m_state->stopRequested || !m_state->fatalError.empty())
                return true;
        }
        // A fatal error also ends the loops, so callers observing "should stop"
        // must see the same answer as the thread bodies do.
        return HasRenderFatalError();
    }

    void RenderingThreadSystem::RequestStop()
    {
        std::lock_guard lock(m_state->mutex);
        m_state->stopRequested = true;
    }

    void RenderingThreadSystem::MarkFatalError(std::string diagnostic)
    {
        ReportRenderFatalError(std::move(diagnostic));
    }

    std::string RenderingThreadSystem::GetFatalError() const
    {
        return GetRenderFatalError();
    }

    bool RenderingThreadSystem::HasFatalError() const
    {
        return HasRenderFatalError();
    }

    void ReportRenderFatalError(std::string diagnostic)
    {
        if (diagnostic.empty())
            diagnostic = "Unspecified fatal rendering error";
        ThreadRegistryState& state = RegistryState();
        {
            std::lock_guard lock(state.fatalErrorMutex);
            if (!state.fatalError.empty())
                return;
            state.fatalError = std::move(diagnostic);
        }
        RenderCommandDispatcher::CancelPendingCommands(GetRenderFatalError());
        NLS_LOG_ERROR("RenderingThreadSystem fatal error: " + GetRenderFatalError());
    }

    std::string GetRenderFatalError()
    {
        ThreadRegistryState& state = RegistryState();
        std::lock_guard lock(state.fatalErrorMutex);
        return state.fatalError;
    }

    bool HasRenderFatalError()
    {
        ThreadRegistryState& state = RegistryState();
        std::lock_guard lock(state.fatalErrorMutex);
        return !state.fatalError.empty();
    }

#if defined(NLS_ENABLE_TEST_HOOKS)
    ScopedRenderThreadForTesting::ScopedRenderThreadForTesting()
    {
        if (RenderingThreadSystem::HasActiveRenderThread())
            throw std::logic_error("Test render context requires the dedicated render thread to be joined");
        ++g_renderTestScopeDepth;
    }

    ScopedRenderThreadForTesting::~ScopedRenderThreadForTesting()
    {
        --g_renderTestScopeDepth;
    }

    void ResetRenderFatalErrorForTesting()
    {
        ThreadRegistryState& state = RegistryState();
        std::lock_guard lock(state.fatalErrorMutex);
        state.fatalError.clear();
    }
#endif

    bool RenderingThreadSystem::IsInGameThread()
    {
        ThreadRegistryState& state = RegistryState();
        std::lock_guard lock(state.gameThreadMutex);
        return state.gameThreadId != std::thread::id {} &&
            state.gameThreadId == std::this_thread::get_id();
    }

    bool RenderingThreadSystem::IsInRenderingThread()
    {
#if defined(NLS_ENABLE_TEST_HOOKS)
        if (g_renderTestScopeDepth != 0u)
            return true;
#endif
        return g_inRenderThread;
    }

    size_t RenderingThreadSystem::PumpGameThreadTasks(const size_t maxTasks)
    {
        ThreadRegistryState& state = RegistryState();
        std::vector<TaskGraph*> graphs;
        {
            std::lock_guard lock(state.gameThreadMutex);
            if (state.gameThreadRegistrationCount == 0u ||
                state.gameThreadId != std::this_thread::get_id())
                return 0u;
            for (const auto& entry : state.gameGraphs)
                graphs.push_back(entry.graph);
        }
        size_t executed = 0u;
        for (TaskGraph* graph : graphs)
        {
            executed += graph->Pump(NamedThread::Game, maxTasks == 0u ? 0u : maxTasks - executed);
            if (maxTasks != 0u && executed >= maxTasks)
                break;
        }
        return executed;
    }

#if defined(NLS_ENABLE_TEST_HOOKS)
    uint32_t RenderingThreadSystem::GetActiveRenderThreadCountForTesting()
    {
        return RegistryState().activeRenderThreadCount.load(std::memory_order_acquire);
    }
#endif

    bool RenderingThreadSystem::HasActiveRenderThread()
    {
        return RegistryState().activeRenderThreadCount.load(std::memory_order_acquire) != 0u;
    }

    bool RenderingThreadSystem::IsInRhiThread()
    {
        // Tasks mode has no dedicated thread, so the logical lane scope decides.
        return g_rhiLaneDepth > 0u || g_inDedicatedRhiThread;
    }

    NLS::Base::TaskGraph::NamedThread RenderingThreadSystem::GetCurrentNamedThread()
    {
        if (IsInRenderingThread())
            return NamedThread::Render;
        if (IsInRhiThread())
            return NamedThread::Rhi;
        if (IsInGameThread())
            return NamedThread::Game;
        return NamedThread::Any;
    }
}
