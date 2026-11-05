#include "TaskGraph/TaskGraphSystem.h"

#include <memory>
#include <mutex>

namespace NLS::Base::TaskGraph
{
    namespace
    {
        std::mutex g_globalTaskGraphMutex;
        // Intentionally process-lifetime. Drivers and named-thread loops keep a
        // non-owning pointer, so static destruction order must not invalidate it.
        TaskGraph* g_globalTaskGraph = nullptr;
    }

    bool InitializeGlobalTaskGraph(const TaskGraphConfig& config)
    {
        TaskGraph* graph;
        {
            std::lock_guard lock(g_globalTaskGraphMutex);
            if (g_globalTaskGraph == nullptr)
            {
                // Initial startup cannot wait for this unpublished graph's
                // tasks. Publish only after it finishes so concurrent readers
                // cannot observe a half-initialized singleton.
                auto initialGraph = std::make_unique<TaskGraph>(config);
                const bool started = initialGraph->Start();
                g_globalTaskGraph = initialGraph.release();
                return started;
            }
            graph = g_globalTaskGraph;
        }
        // Start and shutdown can wait for callbacks which inspect the global
        // graph. Never hold the global pointer lock across lifecycle operations.
        return graph->Start();
    }

    TaskGraph& GetGlobalTaskGraph()
    {
        TaskGraph* graph;
        {
            std::lock_guard lock(g_globalTaskGraphMutex);
            if (g_globalTaskGraph == nullptr)
            {
                auto initialGraph = std::make_unique<TaskGraph>();
                (void)initialGraph->Start();
                g_globalTaskGraph = initialGraph.release();
            }
            graph = g_globalTaskGraph;
        }
        // Getting a stopped graph is observational. Only explicit Initialize
        // restarts it, so a callback cannot wait for its own shutdown to finish.
        return *graph;
    }

    bool IsGlobalTaskGraphStarted()
    {
        TaskGraph* graph;
        {
            std::lock_guard lock(g_globalTaskGraphMutex);
            graph = g_globalTaskGraph;
        }
        return graph != nullptr && graph->IsStarted();
    }

    void ShutdownGlobalTaskGraph(const TaskGraphShutdownMode mode)
    {
        TaskGraph* graph;
        {
            std::lock_guard lock(g_globalTaskGraphMutex);
            graph = g_globalTaskGraph;
        }
        if (graph != nullptr)
            graph->Shutdown(mode);
    }

#if defined(NLS_ENABLE_TEST_HOOKS)
    void ResetGlobalTaskGraphForTesting()
    {
        ShutdownGlobalTaskGraph(TaskGraphShutdownMode::Immediate);
    }
#endif
}
