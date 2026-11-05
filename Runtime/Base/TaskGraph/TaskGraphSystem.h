#pragma once

#include "BaseDef.h"
#include "TaskGraph/TaskGraph.h"

namespace NLS::Base::TaskGraph
{
    NLS_BASE_API bool InitializeGlobalTaskGraph(const TaskGraphConfig& config = {});
    // Lazily starts on first creation; after explicit shutdown, use Initialize
    // to restart. Reading the process-lifetime graph never waits on shutdown.
    NLS_BASE_API TaskGraph& GetGlobalTaskGraph();
    NLS_BASE_API bool IsGlobalTaskGraphStarted();
    NLS_BASE_API void ShutdownGlobalTaskGraph(
        TaskGraphShutdownMode mode = TaskGraphShutdownMode::DrainAcceptedWork);
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS_BASE_API void ResetGlobalTaskGraphForTesting();
#endif
}
