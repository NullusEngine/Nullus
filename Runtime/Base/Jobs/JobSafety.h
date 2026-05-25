#pragma once

#include "BaseDef.h"

namespace NLS::Base::Jobs
{
    class NLS_BASE_API DisallowJobSyncWaitScope
    {
    public:
        DisallowJobSyncWaitScope();
        ~DisallowJobSyncWaitScope();

        DisallowJobSyncWaitScope(const DisallowJobSyncWaitScope&) = delete;
        DisallowJobSyncWaitScope& operator=(const DisallowJobSyncWaitScope&) = delete;
        DisallowJobSyncWaitScope(DisallowJobSyncWaitScope&&) = delete;
        DisallowJobSyncWaitScope& operator=(DisallowJobSyncWaitScope&&) = delete;
    };

    NLS_BASE_API bool IsJobSyncWaitDisallowedForCurrentThread();
}
