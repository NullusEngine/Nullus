#pragma once

#include "BaseDef.h"
#include "TaskGraph/TaskGraph.h"

namespace NLS::Base::TaskGraph
{
    NLS_BASE_API bool InitializeGlobalTaskGraph(const TaskGraphConfig& config = {});
    NLS_BASE_API TaskGraph& GetGlobalTaskGraph();
    NLS_BASE_API bool IsGlobalTaskGraphStarted();
    NLS_BASE_API void ShutdownGlobalTaskGraph(
        TaskGraphShutdownMode mode = TaskGraphShutdownMode::DrainAcceptedWork);
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS_BASE_API void ResetGlobalTaskGraphForTesting();
#endif
}
