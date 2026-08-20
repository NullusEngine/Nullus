#pragma once

#include "ScriptTypes.h"

#include <memory>
#include <optional>
#include <span>

namespace NLS::Scripting
{
class NLS_SCRIPTING_API IScriptBackend
{
public:
    virtual ~IScriptBackend() = default;

    virtual ScriptBackendId GetBackendId() const = 0;
    virtual ScriptLanguage GetLanguage() const = 0;
    virtual ScriptBackendCapabilities GetCapabilities() const { return {}; }
    virtual ScriptStatus Initialize(const ScriptApiDatabase& api) = 0;
    virtual void Shutdown() = 0;
    virtual ScriptStatus CaptureFrame(const ScriptFrameContext&) { return ScriptStatus::Ok(); }
    virtual ScriptStatus LoadScript(const ScriptAsset& asset) = 0;
    virtual ScriptStatus UnloadScript(const NLS::Core::Assets::AssetId& assetId) = 0;
    virtual ScriptStatus CreateInstance(
        const ScriptAsset& asset,
        NativeObjectHandle owner,
        ScriptInstanceHandle& output) = 0;
    virtual ScriptStatus DestroyInstance(ScriptInstanceHandle instance) = 0;
    virtual ScriptStatus Invoke(
        ScriptInstanceHandle instance,
        ScriptCallback callback,
        const ScriptInvocationContext& context) = 0;
    virtual ScriptStatus InvokeBatch(
        ScriptCallback callback,
        std::span<const ScriptInstanceHandle> instances,
        const ScriptInvocationContext& context);
    virtual ScriptStatus Reload(
        const NLS::Core::Assets::AssetId& assetId,
        const ScriptApiDatabase& api) = 0;
    // Script assets can expose a descriptor that is not part of the portable
    // Native API manifest (for example a generated C# Behaviour type).  The
    // default keeps existing backends source-compatible; runtimes query this
    // only after the regular API database lookup has failed.
    virtual const ScriptTypeDescriptor* FindScriptType(ScriptTypeId) const { return nullptr; }
    virtual bool GetField(ScriptInstanceHandle instance, ScriptFieldId field, ScriptValue& output) = 0;
    virtual bool SetField(ScriptInstanceHandle instance, ScriptFieldId field, const ScriptValue& value) = 0;
};

// Optional diagnostics are deliberately kept outside IScriptBackend so the
// fixed backend contract remains source and ABI compatible. Backends expose
// the latest structured failure immediately after a failed call; the runtime
// consumes it before the next backend entry point is invoked.
class NLS_SCRIPTING_API IScriptDiagnosticProvider
{
public:
    virtual ~IScriptDiagnosticProvider() = default;
    virtual std::optional<ScriptError> ConsumeLastDiagnostic() = 0;
};
}
