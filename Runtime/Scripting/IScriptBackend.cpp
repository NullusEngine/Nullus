#include "IScriptBackend.h"

namespace NLS::Scripting
{
ScriptStatus IScriptBackend::InvokeBatch(
    ScriptCallback callback,
    std::span<const ScriptInstanceHandle> instances,
    const ScriptInvocationContext& context)
{
    ScriptStatus firstError = ScriptStatus::Ok();
    for (size_t index = 0; index < instances.size(); ++index)
    {
        auto instanceContext = context;
        if (context.batchOwners.size() == instances.size())
            instanceContext.owner = context.batchOwners[index];
        const auto status = Invoke(instances[index], callback, instanceContext);
        // A failing script must not prevent the remaining instances in the
        // same frame from running.  Preserve the first error for the caller
        // after the complete batch has been attempted.
        if (!status.Succeeded() && firstError.Succeeded())
            firstError = status;
    }
    return firstError;
}
}
