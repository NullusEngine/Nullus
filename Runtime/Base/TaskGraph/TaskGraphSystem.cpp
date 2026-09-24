#include "TaskGraph/TaskGraphSystem.h"

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
        std::lock_guard lock(g_globalTaskGraphMutex);
        if (g_globalTaskGraph != nullptr && g_globalTaskGraph->IsStarted())
            return true;
        if (g_globalTaskGraph == nullptr)
            g_globalTaskGraph = new TaskGraph(config);
        return g_globalTaskGraph->Start();
    }

    TaskGraph& GetGlobalTaskGraph()
    {
        std::lock_guard lock(g_globalTaskGraphMutex);
        if (g_globalTaskGraph == nullptr)
            g_globalTaskGraph = new TaskGraph();
        if (!g_globalTaskGraph->IsStarted())
            (void)g_globalTaskGraph->Start();
        return *g_globalTaskGraph;
    }

    bool IsGlobalTaskGraphStarted()
    {
        std::lock_guard lock(g_globalTaskGraphMutex);
        return g_globalTaskGraph != nullptr && g_globalTaskGraph->IsStarted();
    }

    void ShutdownGlobalTaskGraph(const TaskGraphShutdownMode mode)
    {
        std::lock_guard lock(g_globalTaskGraphMutex);
        if (g_globalTaskGraph == nullptr)
            return;
        g_globalTaskGraph->Shutdown(mode);
    }

#if defined(NLS_ENABLE_TEST_HOOKS)
    void ResetGlobalTaskGraphForTesting()
    {
        std::lock_guard lock(g_globalTaskGraphMutex);
        if (g_globalTaskGraph != nullptr)
            g_globalTaskGraph->Shutdown(TaskGraphShutdownMode::Immediate);
    }
#endif
}
