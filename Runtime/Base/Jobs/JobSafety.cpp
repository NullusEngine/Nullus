#include "Jobs/JobSafety.h"

namespace NLS::Base::Jobs
{
    namespace
    {
        thread_local unsigned g_disallowSyncWaitDepth = 0u;
    }

    DisallowJobSyncWaitScope::DisallowJobSyncWaitScope()
    {
        ++g_disallowSyncWaitDepth;
    }

    DisallowJobSyncWaitScope::~DisallowJobSyncWaitScope()
    {
        if (g_disallowSyncWaitDepth > 0u)
            --g_disallowSyncWaitDepth;
    }

    bool IsJobSyncWaitDisallowedForCurrentThread()
    {
        return g_disallowSyncWaitDepth > 0u;
    }
}
