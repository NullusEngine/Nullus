#include "ScriptScheduler.h"

namespace NLS::Scripting
{
ScriptStatus ScriptScheduler::Flush(ScriptRuntime& runtime, ScriptCallback callback, const ScriptFrameContext& frame)
{
    const auto captureStatus = runtime.CaptureFrame(frame);
    if (!captureStatus.Succeeded())
    {
        m_entries.clear();
        return captureStatus;
    }
    ScriptStatus firstError = ScriptStatus::Ok();
    size_t offset = 0;
    while (offset < m_entries.size())
    {
        const auto backendId = m_entries[offset].instance.backendId;
        size_t end = offset + 1;
        while (end < m_entries.size() && m_entries[end].instance.backendId == backendId)
            ++end;
        m_batchInstances.clear();
        m_batchOwners.clear();
        m_batchInstances.reserve(end - offset);
        m_batchOwners.reserve(end - offset);
        for (size_t index = offset; index < end; ++index)
        {
            m_batchInstances.push_back(m_entries[index].instance);
            m_batchOwners.push_back(m_entries[index].owner);
        }
        ScriptInvocationContext context;
        context.frame = frame;
        context.owner = m_entries[offset].owner;
        context.batchOwners = m_batchOwners;
        const auto status = runtime.InvokeBatch(callback, m_batchInstances, context);
        if (!status.Succeeded() && firstError.Succeeded())
            firstError = status;
        offset = end;
    }
    m_entries.clear();
    return firstError;
}
}
