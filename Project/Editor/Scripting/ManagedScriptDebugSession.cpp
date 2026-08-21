#include "ManagedScriptDebugSession.h"

#include <Scripting/CoreClrScriptBackend.h>

#include <Debug/Logger.h>

#include <cstdlib>
#include <sstream>
#include <utility>

namespace NLS::Editor::Scripting
{
namespace
{
std::filesystem::path EnvironmentPath(const char* name)
{
    const auto* value = std::getenv(name);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

std::filesystem::path ResolveDotnetRoot(const std::filesystem::path& projectRoot)
{
    const auto dotnet = ManagedScriptBuildService::ResolveRepositoryDotnet(projectRoot);
    if (dotnet != std::filesystem::path("dotnet") && !dotnet.parent_path().empty())
        return dotnet.parent_path();

    for (const char* name : {
        "NLS_DOTNET_ROOT",
        "DOTNET_ROOT_X64",
        "DOTNET_ROOT",
        "DOTNET_ROOT(x86)"})
    {
        if (const auto environment = EnvironmentPath(name); !environment.empty())
            return environment;
    }
    return {};
}

bool DiagnosticsDisabled()
{
    const auto disabled = [](const char* name)
    {
        const auto* value = std::getenv(name);
        return value != nullptr && std::string(value) == "0";
    };
    return disabled("DOTNET_EnableDiagnostics") || disabled("COMPlus_EnableDiagnostics");
}
}

ManagedScriptDebugSession::ManagedScriptDebugSession(
    NLS::Scripting::ScriptRuntime& runtime,
    std::filesystem::path projectRoot)
    : m_runtime(&runtime)
    , m_projectRoot(std::move(projectRoot))
{
}

bool ManagedScriptDebugSession::StartBuild()
{
    if (!m_runtime || m_projectRoot.empty())
        return false;

    // A file watcher can observe a second save after the worker has finished
    // but before the next Editor tick consumes its future. Apply that result
    // before replacing the future, otherwise the completed build is lost.
    if (m_buildFuture.valid())
    {
        if (IsBuildInProgress())
            return false;
        Poll();
        if (IsBuildInProgress())
            return true;
    }

    m_buildFuture = m_buildService.Start({m_projectRoot, {}, {}, "Debug"});
    m_preparation.state = ManagedDebugPreparationState::Building;
    return m_buildFuture.valid();
}

bool ManagedScriptDebugSession::RequestPrepareForAttach()
{
    if (!m_runtime || m_projectRoot.empty())
    {
        m_preparation.state = ManagedDebugPreparationState::Failed;
        m_preparation.status = NLS::Scripting::ScriptStatus::Error(
            NLS::Scripting::ScriptStatusCode::InvalidArgument,
            "C# debug preparation has no project runtime.");
        return false;
    }
    m_prepareRequested = true;
    m_enabled = true;
    if (DiagnosticsDisabled())
    {
        m_preparation.state = ManagedDebugPreparationState::Failed;
        m_preparation.status = NLS::Scripting::ScriptStatus::Error(
            NLS::Scripting::ScriptStatusCode::BackendUnavailable,
            "CoreCLR diagnostics are disabled by DOTNET_EnableDiagnostics or COMPlus_EnableDiagnostics.");
        return false;
    }
    if (IsBuildInProgress())
    {
        m_preparation.state = ManagedDebugPreparationState::Building;
        return true;
    }
    if (const auto* backend = dynamic_cast<const NLS::Scripting::CoreClrScriptBackend*>(
            m_runtime->GetBackend(NLS::Scripting::ScriptLanguage::CSharp));
        backend != nullptr && backend->GetHost().IsLoaded() && !m_lastBuild.assembly.empty())
    {
        m_preparation.state = ManagedDebugPreparationState::Ready;
        m_preparation.status = NLS::Scripting::ScriptStatus::Ok();
        return true;
    }
    if (!StartBuild())
    {
        m_preparation.state = ManagedDebugPreparationState::Failed;
        m_preparation.status = NLS::Scripting::ScriptStatus::Error(
            NLS::Scripting::ScriptStatusCode::CompilationFailed,
            "Unable to queue the repository-local C# Debug build.");
        return false;
    }
    return true;
}

bool ManagedScriptDebugSession::RequestBuild()
{
    if (!m_enabled || !m_runtime || m_projectRoot.empty())
        return false;
    if (IsBuildInProgress())
    {
        m_rebuildRequested = true;
        return true;
    }
    return StartBuild();
}

bool ManagedScriptDebugSession::IsBuildInProgress() const
{
    if (!m_buildFuture.valid())
        return false;
    return m_buildFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

void ManagedScriptDebugSession::Poll()
{
    if (!m_buildFuture.valid() || IsBuildInProgress())
        return;

    m_lastBuild = m_buildFuture.get();
    if (!m_lastBuild.succeeded)
    {
        NLS_LOG_ERROR("Managed C# Debug build failed: " + m_lastBuild.output);
        m_lastStatus = NLS::Scripting::ScriptStatus::Error(
            NLS::Scripting::ScriptStatusCode::CompilationFailed,
            "Managed C# Debug build failed. The previous assembly remains active.");
        if (m_prepareRequested)
        {
            m_preparation.state = ManagedDebugPreparationState::Failed;
            m_preparation.status = m_lastStatus;
        }
    }
    else
    {
        if (m_prepareRequested)
            m_preparation.state = ManagedDebugPreparationState::LoadingRuntime;
        m_lastStatus = ApplyBuild(m_lastBuild);
        if (m_lastStatus.Succeeded())
        {
            NLS_LOG_INFO("Managed C# Debug build activated: " + m_lastBuild.assembly.string());
            m_preparation.assembly = m_lastBuild.assembly;
            m_preparation.symbols = m_lastBuild.symbols;
            m_preparation.status = m_lastStatus;
            m_preparation.state = m_prepareRequested
                ? ManagedDebugPreparationState::Ready
                : ManagedDebugPreparationState::Idle;
        }
        else
        {
            NLS_LOG_ERROR("Managed C# Debug build could not activate: " + m_lastStatus.message);
            if (m_prepareRequested)
            {
                m_preparation.state = ManagedDebugPreparationState::Failed;
                m_preparation.status = m_lastStatus;
            }
        }
    }

    // A save that arrived while dotnet was compiling must not be dropped. The
    // next build starts only after the current result has been committed (or
    // rejected), preserving the old assembly on a failed transaction.
    if (m_rebuildRequested && m_enabled)
    {
        m_rebuildRequested = false;
        if (!StartBuild())
            NLS_LOG_WARNING("Managed C# rebuild requested after a source change but could not be queued.");
    }
}

NLS::Scripting::ScriptStatus ManagedScriptDebugSession::ApplyBuild(
    const ManagedScriptBuildResult& result)
{
    if (!m_runtime)
        return NLS::Scripting::ScriptStatus::Error(
            NLS::Scripting::ScriptStatusCode::InvalidArgument,
            "C# debug session has no ScriptRuntime.");
    if (result.assembly.empty() || result.runtimeConfig.empty() || result.symbols.empty())
        return NLS::Scripting::ScriptStatus::Error(
            NLS::Scripting::ScriptStatusCode::AssetNotLoaded,
            "Managed C# Debug build did not produce the DLL, PDB, or runtimeconfig artifacts.");

    const auto dotnetRoot = ResolveDotnetRoot(m_projectRoot);
    auto* backend = dynamic_cast<NLS::Scripting::CoreClrScriptBackend*>(
        m_runtime->GetBackend(NLS::Scripting::ScriptLanguage::CSharp));
    if (!backend)
    {
        auto replacement = std::make_unique<NLS::Scripting::CoreClrScriptBackend>();
        replacement->SetDotnetRoot(dotnetRoot);
        replacement->SetHostArtifacts(result.runtimeConfig, result.assembly);
        return m_runtime->RegisterBackend(std::move(replacement));
    }

    backend->SetDotnetRoot(dotnetRoot);
    if (!m_runtime->IsInitialized())
    {
        backend->SetHostArtifacts(result.runtimeConfig, result.assembly);
        return NLS::Scripting::ScriptStatus::Ok();
    }

    return backend->ReloadProjectAssembly(result.assembly, m_runtime->GetApi());
}
}
