#pragma once

#include "IScriptBackend.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Scripting
{
// Deterministic backend used by the runtime tests and by editor tooling that
// needs to exercise lifecycle routing without loading a language VM.
class NLS_SCRIPTING_API FakeScriptBackend final : public IScriptBackend
{
public:
    explicit FakeScriptBackend(
        ScriptBackendId id = {0xF001},
        ScriptLanguage language = ScriptLanguage::Lua);

    ScriptBackendId GetBackendId() const override { return m_id; }
    ScriptLanguage GetLanguage() const override { return m_language; }
    ScriptBackendCapabilities GetCapabilities() const override
    {
        return {true, true, true, true};
    }

    ScriptStatus Initialize(const ScriptApiDatabase& api) override;
    void Shutdown() override;
    ScriptStatus CaptureFrame(const ScriptFrameContext& frame) override;
    ScriptStatus LoadScript(const ScriptAsset& asset) override;
    ScriptStatus UnloadScript(const NLS::Core::Assets::AssetId& assetId) override;
    ScriptStatus CreateInstance(const ScriptAsset& asset, NativeObjectHandle owner, ScriptInstanceHandle& output) override;
    ScriptStatus DestroyInstance(ScriptInstanceHandle instance) override;
    ScriptStatus Invoke(ScriptInstanceHandle instance, ScriptCallback callback, const ScriptInvocationContext& context) override;
    ScriptStatus InvokeBatch(
        ScriptCallback callback,
        std::span<const ScriptInstanceHandle> instances,
        const ScriptInvocationContext& context) override;
    ScriptStatus Reload(const NLS::Core::Assets::AssetId& assetId, const ScriptApiDatabase& api) override;
    bool GetField(ScriptInstanceHandle instance, ScriptFieldId field, ScriptValue& output) override;
    bool SetField(ScriptInstanceHandle instance, ScriptFieldId field, const ScriptValue& value) override;

    using InvocationObserver = std::function<void(ScriptInstanceHandle, ScriptCallback, const ScriptInvocationContext&)>;
    void SetInvocationObserver(InvocationObserver observer) { m_observer = std::move(observer); }
    const std::vector<ScriptCallback>& GetCallbacks() const { return m_callbacks; }
    const std::vector<ScriptFrameContext>& GetFrames() const { return m_frames; }
    size_t GetBatchCallCount() const { return m_batchCallCount; }
    size_t GetBatchInstanceCount() const { return m_batchInstanceCount; }
    bool WasDestroyed(ScriptInstanceHandle instance) const { return m_destroyed.contains(instance); }
    bool IsInitialized() const { return m_initialized; }
    void ClearTrace();

private:
    struct Instance
    {
        ScriptAsset asset;
        NativeObjectHandle owner;
        SerializedScriptFields fields;
    };

    ScriptBackendId m_id;
    ScriptLanguage m_language;
    ScriptApiDatabase m_api;
    std::unordered_map<NLS::Core::Assets::AssetId, ScriptAsset> m_scripts;
    std::unordered_map<ScriptInstanceHandle, Instance> m_instances;
    std::unordered_set<ScriptInstanceHandle> m_destroyed;
    std::vector<ScriptCallback> m_callbacks;
    std::vector<ScriptFrameContext> m_frames;
    InvocationObserver m_observer;
    size_t m_batchCallCount = 0;
    size_t m_batchInstanceCount = 0;
    uint32_t m_nextIndex = 1;
    uint16_t m_nextGeneration = 1;
    bool m_initialized = false;
};
}
