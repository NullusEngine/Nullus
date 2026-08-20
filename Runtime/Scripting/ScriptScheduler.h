#pragma once

#include "ScriptRuntime.h"

#include <vector>

namespace NLS::Scripting
{
class NLS_SCRIPTING_API ScriptScheduler final
{
public:
    struct Entry
    {
        ScriptInstanceHandle instance;
        NativeObjectHandle owner;
    };

    void Clear() { m_entries.clear(); }
    void Enqueue(ScriptInstanceHandle instance, NativeObjectHandle owner = {})
    {
        if (instance.IsValid())
            m_entries.push_back({instance, owner});
    }
    size_t GetQueuedCount() const { return m_entries.size(); }

    // The queue is intentionally stable.  ScriptRuntime only combines
    // contiguous backend ranges, so a C# -> Lua -> C# ordering remains exact.
    ScriptStatus Flush(ScriptRuntime& runtime, ScriptCallback callback, const ScriptFrameContext& frame);

private:
    std::vector<Entry> m_entries;
    // Reused for every contiguous backend segment.  Keeping these buffers on
    // the scheduler removes the per-segment proxy allocations from steady
    // state frames while preserving the queue's ordering.
    std::vector<ScriptInstanceHandle> m_batchInstances;
    std::vector<NativeObjectHandle> m_batchOwners;
};
}
