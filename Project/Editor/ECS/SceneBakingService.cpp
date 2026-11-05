#include "SceneBakingService.h"
#include <Engine/ECS/TransformHierarchy.h>
#include <Engine/SceneSystem/Scene.h>

namespace NLS::Editor::ECSAuthoring
{
using namespace Engine::ECSIntegration;

SceneBakingService::SceneBakingService(Undo::EditorUndoService& undo,
    std::shared_ptr<NLS::ECS::ComponentRegistry> registry)
    : m_undo(undo), m_registry(registry ? std::move(registry) : std::make_shared<NLS::ECS::ComponentRegistry>()),
      m_pipeline(m_registry)
{
    ResetPreview();
    RegisterTransformBaker(m_pipeline);
    m_listener = m_undo.SubscribeHistoryChanged([this](const Undo::UndoHistoryEvent&)
    {
        // Coalesce Execute, Undo/Redo, asynchronous completion and edit-session
        // notifications. Tick never captures partially applied commands.
        m_dirty = true;
    });
}

SceneBakingService::~SceneBakingService()
{
    m_undo.UnsubscribeHistoryChanged(m_listener);
}

void SceneBakingService::ResetPreview()
{
    m_preview = std::make_unique<BakingPreview>(m_registry);
    RegisterTransformComponents(m_preview->GetWorld());
    RegisterTransformSystem(m_preview->GetWorld());
    m_pipeline.InvalidateCache();
    m_result = {};
    m_dirty = true;
    m_requested.reset();
    m_nextPoll = {};
}

Engine::SceneSystem::Scene* SceneBakingService::ResolveScene() const
{
    return dynamic_cast<Engine::SceneSystem::Scene*>(NLS::Object::IDToPointer(m_scene));
}

std::string SceneBakingService::GetUnavailableReason() const
{
    if (!m_editing || m_undo.GetPlayDomainId() != 0) return "Return to Edit mode to bake the authoring Scene.";
    const auto* scene = ResolveScene();
    if (!scene) return "Open an authoring Scene first.";
    if (scene->IsPlaying()) return "Stop the running Scene before baking.";
    if (scene->IsSerializationReadOnly()) return "Restore unresolved authoring types before baking.";
    if (m_undo.IsPending() || m_undo.HasActiveEditSession() || m_undo.HasActiveTransaction())
        return "Finish the current edit before baking.";
    if (m_undo.IsFrozen() || m_undo.IsPoisoned()) return "Editor history is unavailable during this operation.";
    return {};
}

void SceneBakingService::SetLive(bool live)
{
    if (live && !m_live) m_dirty = true;
    m_live = live;
}

void SceneBakingService::RequestBake(BakingMode mode)
{
    if (!m_requested || mode == BakingMode::Full) m_requested = mode;
}

void SceneBakingService::SetResourceRevisionProvider(
    std::function<std::optional<std::string>(const NLS::Guid&)> provider)
{
    m_resourceRevision = std::move(provider);
    m_dirty = true;
}

bool SceneBakingService::Tick(Engine::SceneSystem::Scene* scene, bool editing, Clock::time_point now)
{
    m_editing = editing;
    if (!editing || m_undo.GetPlayDomainId() != 0) return false;
    const auto id = scene ? scene->GetInstanceID() : InstanceID_None;
    if (m_scene != id)
    {
        m_scene = id;
        ResetPreview();
    }
    if (!GetUnavailableReason().empty()) return false;
    if (!m_requested && (!m_live || (!m_dirty && now < m_nextPoll))) return false;
    // Also poll Capture at a bounded rate: legacy authoring APIs can mutate
    // fields without emitting Undo events. The incremental cache still skips
    // unchanged Bake callbacks and preserves preview handles.
    const auto mode = m_requested.value_or(BakingMode::Incremental);
    m_requested.reset();
    m_nextPoll = now + std::chrono::milliseconds(250);
    m_dirty = false;
    BakeNow(mode);
    return true;
}

bool SceneBakingService::BakeNow(BakingMode mode)
{
    const auto unavailable = GetUnavailableReason();
    if (!unavailable.empty())
    {
        m_result.succeeded = false;
        m_result.diagnostics = {unavailable};
        return false;
    }
    ++m_bakeCount;
    const auto previous = m_result.artifact;
    try
    {
        const auto sources = CaptureSceneBakingSources(*ResolveScene());
        BakingOptions options;
        options.mode = mode;
        options.resourceRevision = m_resourceRevision;
        m_result = m_pipeline.Bake(sources, options);
        if (!m_result.succeeded) return false;
        NLS::ECS::ArchiveLoadOptions load;
        load.resolveResource = [this](const NLS::ECS::ResourceDependency& dependency)
        {
            const auto guid = NLS::Guid::TryParse(dependency.guid);
            if (!guid || !m_resourceRevision) return false;
            const auto revision = m_resourceRevision(*guid);
            return revision && *revision == dependency.hashOrVersion;
        };
        m_preview->Apply(m_result, load);
        m_preview->GetWorld().Systems().RunPhase(NLS::ECS::SystemPhase::LateUpdate, {});
        return true;
    }
    catch (const std::exception& exception)
    {
        m_result.succeeded = false;
        m_result.artifact = previous;
        m_result.diagnostics = {exception.what()};
        return false;
    }
}

bool SceneBakingService::ExportScene(const std::filesystem::path& path,
    const std::function<bool(const std::filesystem::path&)>& import)
{
    if (path.empty() || path.extension() != ".ecsscene")
    {
        m_result.succeeded = false;
        m_result.diagnostics = {"Choose an output file with the .ecsscene extension."};
        return false;
    }
    if (!BakeNow(BakingMode::Full)) return false;
    try
    {
        NLS::ECS::WorldArchive::WriteFile(path, *m_result.artifact);
        if (import && !import(path))
            throw std::runtime_error("The ECS Scene was written, but asset import failed. See the import diagnostics.");
        return true;
    }
    catch (const std::exception& exception)
    {
        m_result.succeeded = false;
        m_result.diagnostics = {exception.what()};
        return false;
    }
}
}
