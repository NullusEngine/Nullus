#pragma once

#include "ManagedScriptBuildService.h"

#include <Scripting/ScriptRuntime.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>

namespace NLS::Editor::Scripting
{
enum class ManagedDebugPreparationState : uint8_t
{
    Idle,
    Building,
    LoadingRuntime,
    Ready,
    Failed
};

struct ManagedDebugPreparationResult
{
    ManagedDebugPreparationState state = ManagedDebugPreparationState::Idle;
    std::filesystem::path assembly;
    std::filesystem::path symbols;
    std::string assemblyHash;
    NLS::Scripting::ScriptStatus status;
};

// Main-thread coordinator for the Editor C# debugger.  The build itself runs
// on the repository-local .NET SDK worker, while all runtime/ALC operations
// are applied from Poll() at the start of an Editor frame.
class ManagedScriptDebugSession final
{
public:
    ManagedScriptDebugSession(
        NLS::Scripting::ScriptRuntime& runtime,
        std::filesystem::path projectRoot);
    ~ManagedScriptDebugSession() = default;

    ManagedScriptDebugSession(const ManagedScriptDebugSession&) = delete;
    ManagedScriptDebugSession& operator=(const ManagedScriptDebugSession&) = delete;

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    // Starts one deterministic Debug build.  The caller should invoke Poll()
    // once per Editor frame until IsBuildInProgress() becomes false.
    bool StartBuild();
    // Request the state required by an IDE debugger. This is idempotent and
    // never enters Play or creates script instances.
    bool RequestPrepareForAttach();
    // Queue a build for a source change.  Changes received while a build is
    // running are coalesced and rebuilt once the current transaction finishes.
    bool RequestBuild();
    void Poll();
    bool IsBuildInProgress() const;

    const ManagedScriptBuildResult& GetLastBuild() const { return m_lastBuild; }
    const NLS::Scripting::ScriptStatus& GetLastStatus() const { return m_lastStatus; }
    const ManagedDebugPreparationResult& GetPreparation() const { return m_preparation; }

private:
    NLS::Scripting::ScriptStatus ApplyBuild(const ManagedScriptBuildResult& result);

    NLS::Scripting::ScriptRuntime* m_runtime = nullptr;
    std::filesystem::path m_projectRoot;
    ManagedScriptBuildService m_buildService;
    std::future<ManagedScriptBuildResult> m_buildFuture;
    ManagedScriptBuildResult m_lastBuild;
    NLS::Scripting::ScriptStatus m_lastStatus;
    bool m_enabled = false;
    bool m_rebuildRequested = false;
    bool m_prepareRequested = false;
    ManagedDebugPreparationResult m_preparation;
};
}
