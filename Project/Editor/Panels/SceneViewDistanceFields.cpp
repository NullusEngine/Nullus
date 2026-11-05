#include "Panels/SceneView.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include "Assets/AssetMeta.h"
#include "Assets/AssetImporterFacade.h"
#include "Assets/DistanceFieldSceneBuild.h"
#include "Project/Editor/Assets/DistanceFieldBuildService.h"
#include "Debug/Logger.h"
#include "ImGui/imgui.h"
#include "Panels/AssetBrowser.h"
#include "Rendering/Assets/DistanceFieldBuilder.h"
#include "Rendering/Context/DriverAccess.h"
#include "UI/UIManager.h"

namespace NLS::Editor::Panels
{
bool SceneView::BuildSceneDistanceFields(bool forceRebuild, uint32_t mipMask)
{
    auto& browser = EDITOR_PANEL(AssetBrowser, "Asset Browser");
    const auto database = browser.GetAssetDatabaseSnapshot();
    if (!GetScene() || !database) return false;
    const auto paths = Assets::CollectSceneDistanceFieldModelAssets(*GetScene(), *database);
    m_distanceFieldBuildAssets.clear();
    for (const auto& path : paths)
    {
        const auto source = std::filesystem::path(EDITOR_CONTEXT(projectPath)) / path;
        const auto meta = NLS::Core::Assets::AssetMeta::Load(source.string() + ".meta");
        if (!meta || !Render::DistanceField::ReadDistanceFieldBuildSettings(
                meta->settings, EDITOR_CONTEXT(projectPath)).Enabled()) continue;
        Assets::DistanceFieldBuildRequest request;
        request.projectRoot=EDITOR_CONTEXT(projectPath);request.assetPath=path;
        request.forceRebuild=forceRebuild;request.mipMask=mipMask;
        if(const auto manifest=database->GetArtifactManifestForAssetPath(path))
            request.sourceIdentityHint=Render::DistanceField::DistanceFieldSourceManifestIdentity(*manifest);
        if (Assets::DistanceFieldBuildService::Queue(std::move(request)))
            m_distanceFieldBuildAssets.push_back(path);
    }
    m_distanceFieldBuildMessage = m_distanceFieldBuildAssets.empty()
        ? "No enabled source models found. Enable generation in project or model import settings."
        : "Queued " + std::to_string(m_distanceFieldBuildAssets.size()) + " distance field build(s). See Background Tasks for progress and errors.";
    ++m_distanceFieldDebugRevision;
    InvalidateStaticFrameCache();
    return !m_distanceFieldBuildAssets.empty();
}

void SceneView::DrawDistanceFieldBuildControls()
{
    auto& project = EDITOR_CONTEXT(projectSettings);
    bool generation = project.GetOrDefault<bool>("generate_mesh_distance_fields", false);
    if (ImGui::Checkbox("Generate Mesh Distance Fields", &generation))
    {
        if (project.IsKeyExisting("generate_mesh_distance_fields")) project.Set<bool>("generate_mesh_distance_fields", generation);
        else project.Add<bool>("generate_mesh_distance_fields", generation);
        project.Rewrite();
    }
    const bool building = std::any_of(m_distanceFieldBuildAssets.begin(), m_distanceFieldBuildAssets.end(),
        [&](const auto& path) { return Assets::DistanceFieldBuildService::IsBuilding(EDITOR_CONTEXT(projectPath),path); });
    ImGui::BeginDisabled(building);
    if (ImGui::Button("Build Missing / Outdated")) BuildSceneDistanceFields();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Builds distance fields from published LOD0 geometry. Current meshes and mips are reused.");
    if (ImGui::Button("Build Coarse Preview")) BuildSceneDistanceFields(false,4);
    if (ImGui::Button("Force Rebuild Model Fields")) BuildSceneDistanceFields(true);
    ImGui::EndDisabled();
    ImGui::TextWrapped("Model fields build in the background from imported geometry. Rebuild Global Pages refreshes the global cache.");
    if (!m_distanceFieldBuildMessage.empty()) ImGui::TextWrapped("%s", m_distanceFieldBuildMessage.c_str());
    for(const auto& path:m_distanceFieldBuildAssets)if(const auto job=Assets::DistanceFieldBuildService::FindBuildJob(EDITOR_CONTEXT(projectPath),path))
    {
        std::lock_guard lock(job->mutex);
        const auto& s=job->running.load()?job->progress.stats:job->result.model.stats;
        ImGui::TextWrapped("%s",path.c_str());
        ImGui::Text("Built / cached / current mips: %llu / %llu / %llu",static_cast<unsigned long long>(s.builtMips),
            static_cast<unsigned long long>(s.cachedMips),static_cast<unsigned long long>(job->result.model.skippedMips));
        ImGui::Text("Backend: %s  BVH: %.1f ms",s.backend.c_str(),s.bvhMs);
        ImGui::Text("Nearest / sign CPU: %.1f / %.1f ms",s.distanceMs,s.signMs);
        if(job->running.load())ImGui::Text("Mesh: %s  Mip: %u  %u / %u",job->progress.meshKey.c_str(),job->progress.mip,job->progress.completed,job->progress.total);
        else if(!job->result.error.empty())ImGui::TextWrapped("%s",job->result.error.c_str());
    }
}

void SceneView::DrawDistanceFieldStatusOverlay()
{
    namespace df = Render::DistanceField;
    if (m_distanceFieldDebug.visualization == df::DistanceFieldVisualization::Off || !HasViewportImageBounds()) return;
    auto* renderer = dynamic_cast<Engine::Rendering::BaseSceneRenderer*>(m_renderer.get());
    if (!renderer) return;
    const auto stats = renderer->GetDistanceFieldStats();
    std::string message;
    const auto device = Render::Context::DriverRendererAccess::GetExplicitDevice(*EDITOR_CONTEXT(driver));
    if (!device || !device->GetCapabilities().SupportsFeature(Render::RHI::RHIDeviceFeature::DistanceFields))
        message = "Distance fields require the Windows DX12 renderer.";
    else if (std::any_of(m_distanceFieldBuildAssets.begin(), m_distanceFieldBuildAssets.end(),
        [&](const auto& path) { return Assets::DistanceFieldBuildService::IsBuilding(EDITOR_CONTEXT(projectPath),path); }))
        message = "Building mesh distance fields... See Background Tasks for progress.";
    else if (!stats.diagnostic.empty()) message = stats.diagnostic;
    else if (stats.pendingLoads || df::DistanceFieldSystem::HasPendingWork())
        message = "Loading mesh distance fields...";
    else if (!stats.objects)
        message = "No mesh distance fields are available for this scene.\nOpen Draw Mode > Distance Field Settings, enable generation and Build Missing / Outdated.";
    const bool traceSteps = m_distanceFieldDebug.channel == df::DistanceFieldDebugChannel::Steps;
    if (message.empty() && !traceSteps) return;
    const float scale = NLS_SERVICE(UI::UIManager).GetScale();
    const auto min = GetViewportImageMin(), max = GetViewportImageMax();
    auto* draw = ImGui::GetWindowDrawList();
    if (traceSteps)
    {
        const float width = std::min(300 * scale, max.x - min.x - 32 * scale);
        if (width > 120 * scale)
        {
            const float x = min.x + 16 * scale, y = max.y - 64 * scale;
            const float barY = y + 26 * scale, barHeight = 10 * scale;
            const std::array<ImVec4, 5> colors{{{.16f,.32f,.72f,1}, {.10f,.64f,.74f,1},
                {.24f,.73f,.43f,1}, {.94f,.73f,.20f,1}, {.88f,.20f,.14f,1}}};
            draw->PushClipRect(ImVec2(min.x, min.y), ImVec2(max.x, max.y), true);
            draw->AddRectFilled(ImVec2(x - 8 * scale, y - 6 * scale),
                ImVec2(x + width + 8 * scale, barY + barHeight + 8 * scale), IM_COL32(24,27,32,235), 4 * scale);
            const auto title = "Trace Steps: 1 - " + std::to_string(m_distanceFieldDebug.traceStepsMax);
            draw->AddText(ImVec2(x, y), IM_COL32(235,235,235,255), title.c_str());
            for (size_t i = 0; i + 1 < colors.size(); ++i)
            {
                const auto left = ImGui::ColorConvertFloat4ToU32(colors[i]);
                const auto right = ImGui::ColorConvertFloat4ToU32(colors[i + 1]);
                draw->AddRectFilledMultiColor(ImVec2(x + width * float(i) / 4, barY),
                    ImVec2(x + width * float(i + 1) / 4, barY + barHeight), left, right, right, left);
            }
            draw->PopClipRect();
        }
    }
    if (message.empty()) return;
    const ImVec2 position(min.x + 12 * scale, min.y + 12 * scale);
    const float width = std::min(560 * scale, max.x - position.x - 12 * scale);
    if (width <= 0) return;
    const auto size = ImGui::CalcTextSize(message.c_str(), nullptr, false, width);
    draw->PushClipRect(ImVec2(min.x, min.y), ImVec2(max.x, max.y), true);
    draw->AddRectFilled(ImVec2(position.x - 6 * scale, position.y - 6 * scale),
        ImVec2(position.x + size.x + 6 * scale, position.y + size.y + 6 * scale), IM_COL32(24, 27, 32, 235), 4 * scale);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position, IM_COL32(235, 219, 170, 255), message.c_str(), nullptr, width);
    draw->PopClipRect();
}
}
