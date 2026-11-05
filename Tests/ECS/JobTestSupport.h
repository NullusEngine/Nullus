#pragma once
#include <Base/Jobs/JobSystem.h>
#include <stdexcept>

namespace ECSTest
{
class JobSystemScope final
{
public:
    explicit JobSystemScope(uint32_t workers = 4) : m_owned(!NLS::Base::Jobs::IsJobSystemInitialized())
    {
        NLS::Base::Jobs::JobSystemConfig config; config.workerCount = workers; config.backgroundWorkerCount = 1;
        if (m_owned && !NLS::Base::Jobs::TryInitializeJobSystem(config)) throw std::runtime_error("Test JobSystem initialization failed");
    }
    ~JobSystemScope() { if (m_owned) NLS::Base::Jobs::ShutdownJobSystem(); }
    JobSystemScope(const JobSystemScope&) = delete;
    JobSystemScope& operator=(const JobSystemScope&) = delete;
private:
    bool m_owned;
};
}
