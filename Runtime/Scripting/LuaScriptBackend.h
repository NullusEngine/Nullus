#pragma once

#include "IScriptBackend.h"

#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

struct lua_State;

namespace NLS::Scripting
{
enum class LuaPandaConnectionState : uint8_t
{
    Disabled = 0,
    WaitingForAttach,
    Connected
};

// Lua 5.4 backend boundary.  The VM adapter is deliberately kept behind this
// class so a player can link the pinned Lua 5.4.8 sources while editor tests
// can use the deterministic artifact/field/lifecycle implementation below.
class NLS_SCRIPTING_API LuaScriptBackend final : public IScriptBackend, public IScriptDiagnosticProvider
{
public:
    struct OverloadCandidate
    {
        ScriptMemberId id = 0;
        std::vector<ScriptType> parameters;
    };

    static ScriptStatus ResolveOverload(
        std::span<const OverloadCandidate> candidates,
        std::span<const ScriptValue> arguments,
        ScriptMemberId& output);

    using Callback = std::function<ScriptStatus(
        ScriptInstanceHandle,
        ScriptCallback,
        const ScriptInvocationContext&)>;

    explicit LuaScriptBackend(ScriptBackendId id = {2});

    ScriptBackendId GetBackendId() const override { return m_id; }
    ScriptLanguage GetLanguage() const override { return ScriptLanguage::Lua; }
    ScriptBackendCapabilities GetCapabilities() const override { return {true, true, true, true}; }
    ScriptStatus Initialize(const ScriptApiDatabase& api) override;
    void Shutdown() override;
    ScriptStatus CaptureFrame(const ScriptFrameContext& frame) override;
    ScriptStatus LoadScript(const ScriptAsset& asset) override;
    ScriptStatus UnloadScript(const NLS::Core::Assets::AssetId& assetId) override;
    ScriptStatus CreateInstance(const ScriptAsset& asset, NativeObjectHandle owner, ScriptInstanceHandle& output) override;
    ScriptStatus DestroyInstance(ScriptInstanceHandle instance) override;
    ScriptStatus Invoke(ScriptInstanceHandle instance, ScriptCallback callback, const ScriptInvocationContext& context) override;
    ScriptStatus Reload(const NLS::Core::Assets::AssetId& assetId, const ScriptApiDatabase& api) override;
    bool GetField(ScriptInstanceHandle instance, ScriptFieldId field, ScriptValue& output) override;
    bool SetField(ScriptInstanceHandle instance, ScriptFieldId field, const ScriptValue& value) override;
    std::optional<ScriptError> ConsumeLastDiagnostic() override;

    static constexpr const char* LuaVersion() { return "Lua 5.4.8"; }
    static constexpr const char* ArtifactExtension() { return ".lua54"; }
    void SetCallback(Callback callback) { m_callback = std::move(callback); }
    // Configure the optional debugger without letting an initialization error
    // escape the scripting ABI. When the VM is already running, enabling the
    // debugger starts it immediately; a failed start leaves the debugger off.
    ScriptStatus SetLuaPandaDebugging(bool enabled, std::string host = "127.0.0.1", uint16_t port = 8818);
    bool IsLuaPandaDebuggingEnabled() const { return m_luaPandaEnabled; }
    bool IsLuaPandaDebuggerActive() const { return m_luaPandaEnvironmentReference >= 0; }
    LuaPandaConnectionState GetLuaPandaConnectionState() const { return m_luaPandaConnectionState; }
    bool IsLuaPandaConnected() const { return m_luaPandaConnectionState == LuaPandaConnectionState::Connected; }
    const std::vector<ScriptFrameContext>& GetCapturedFrames() const { return m_frames; }

private:
    struct Artifact
    {
        ScriptAsset asset;
        uint64_t contentHash = 0;
        int moduleReference = -2;
    };
    struct Instance
    {
        ScriptAsset asset;
        NativeObjectHandle owner;
        SerializedScriptFields fields;
        int tableReference = -2;
    };

    static ScriptStatus ValidateSource(const ScriptAsset& asset);

    ScriptStatus LoadModule(const ScriptAsset& asset, Artifact& output);
    ScriptStatus InvokeLua(ScriptInstanceHandle instance, ScriptCallback callback, const ScriptInvocationContext& context);
    const ScriptFieldDescriptor* FindField(const Instance& instance, ScriptFieldId field) const;
    static bool PushValue(lua_State* state, const ScriptValue& value);
    static bool ReadValue(lua_State* state, int index, const ScriptType& expected, ScriptValue& output);
    void ReleaseModule(Artifact& artifact);
    void ReleaseInstance(Instance& instance);
    ScriptStatus InitializeLuaPanda();
    void ShutdownLuaPanda();
    static int LuaPandaConnectSuccessHook(lua_State* state);
    static int LuaPandaDisconnectHook(lua_State* state);
    void InstallLuaPandaHooks(lua_State* state, int environment);

    ScriptBackendId m_id;
    ScriptApiDatabase m_api;
    std::unordered_map<NLS::Core::Assets::AssetId, Artifact> m_artifacts;
    // A replacement is compiled into this staging map first.  Existing
    // instances keep the active prototype until Reload commits the swap.
    std::unordered_map<NLS::Core::Assets::AssetId, Artifact> m_pendingArtifacts;
    std::unordered_map<ScriptInstanceHandle, Instance> m_instances;
    std::unordered_set<ScriptInstanceHandle> m_destroyed;
    std::vector<ScriptFrameContext> m_frames;
    Callback m_callback;
    lua_State* m_state = nullptr;
    float m_lastPublishedTimeScale = 1.0f;
    uint32_t m_nextIndex = 1;
    uint16_t m_nextGeneration = 1;
    bool m_initialized = false;
    std::optional<ScriptError> m_lastDiagnostic;
    bool m_luaPandaEnabled = false;
    std::string m_luaPandaHost = "127.0.0.1";
    uint16_t m_luaPandaPort = 8818;
    int m_luaPandaEnvironmentReference = -2;
    LuaPandaConnectionState m_luaPandaConnectionState = LuaPandaConnectionState::Disabled;
};
}
