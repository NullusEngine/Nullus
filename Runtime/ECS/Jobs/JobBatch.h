#pragma once
#include "ECS/ECSDef.h"
#include <cstddef>
#include <functional>
#include <memory>

namespace NLS::ECS::Detail
{
// A scoped adapter to Base::Jobs. Owns no queue or threads. Every accepted
// callback is joined before its borrowed World/chunk data can leave scope.
class NLS_ECS_API JobBatch final
{
public:
    JobBatch();
    ~JobBatch();
    JobBatch(const JobBatch&) = delete;
    JobBatch& operator=(const JobBatch&) = delete;
    void Add(std::function<void()> function, const char* debugName);
    void AddParallelFor(size_t count, size_t grain, std::function<void(size_t)> function, const char* debugName);
    void Wait();
    size_t GetJobCount() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
