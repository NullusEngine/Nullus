#pragma once

#include <Engine/ECS/Baking.h>
#include "Undo/EditorUndoService.h"
#include <chrono>

namespace NLS::Editor::ECSAuthoring
{
// Editor-owned CPU preview. Capture and publication run on the editor thread;
// Play never receives this World or its entities.
class SceneBakingService final
{
public:
    using Clock = std::chrono::steady_clock;
    explicit SceneBakingService(Undo::EditorUndoService& undo,
        std::shared_ptr<NLS::ECS::ComponentRegistry> registry = {});
    ~SceneBakingService();
    SceneBakingService(const SceneBakingService&) = delete;
    SceneBakingService& operator=(const SceneBakingService&) = delete;

    // Called once per frame, including while Play is active. A scene switch in
    // Edit resets the cache; a Play sandbox leaves the Edit preview untouched.
    bool Tick(Engine::SceneSystem::Scene* scene, bool editing, Clock::time_point now = Clock::now());
    void SetLive(bool live);
    bool IsLive() const { return m_live; }
    void RequestBake(Engine::ECSIntegration::BakingMode mode);
    void SetResourceRevisionProvider(std::function<std::optional<std::string>(const NLS::Guid&)> provider);
    bool BakeNow(Engine::ECSIntegration::BakingMode mode = Engine::ECSIntegration::BakingMode::Incremental);
    bool ExportScene(const std::filesystem::path& path,
        const std::function<bool(const std::filesystem::path&)>& import = {});
    std::string GetUnavailableReason() const;
    Engine::ECSIntegration::BakingPipeline& GetPipeline() { return m_pipeline; }
    Engine::ECSIntegration::BakingPreview& GetPreview() { return *m_preview; }
    const Engine::ECSIntegration::BakingResult& GetLastResult() const { return m_result; }
    uint64_t GetBakeCount() const { return m_bakeCount; }

private:
    Engine::SceneSystem::Scene* ResolveScene() const;
    void ResetPreview();
    Undo::EditorUndoService& m_undo;
    Undo::UndoListenerToken m_listener = 0;
    std::shared_ptr<NLS::ECS::ComponentRegistry> m_registry;
    Engine::ECSIntegration::BakingPipeline m_pipeline;
    std::unique_ptr<Engine::ECSIntegration::BakingPreview> m_preview;
    Engine::ECSIntegration::BakingResult m_result;
    InstanceID m_scene = InstanceID_None;
    bool m_editing = true, m_live = false, m_dirty = true;
    std::optional<Engine::ECSIntegration::BakingMode> m_requested;
    Clock::time_point m_nextPoll {};
    uint64_t m_bakeCount = 0;
    std::function<std::optional<std::string>(const NLS::Guid&)> m_resourceRevision;
};
}
