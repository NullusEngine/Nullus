#include "Properties/CurveValue.h"
#include "VfxEditor.h"
#include "VFX/Editor/VfxStageControlWidgets.h"
#include "VfxLibraryEditor.h"
#include "AssetBrowser.h"
#include "Core/Editor.h"
#include "Core/EditorActions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <future>
#include <functional>
#include <unordered_map>
#include <numeric>
#include <Guid.h>
#include <ServiceLocator.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>
#include <Rendering/Context/Driver.h>
#include <Rendering/Context/DriverAccess.h>
#include <UI/UIManager.h>
#include <UI/Icons/FontAwesomeIconFont.h>
#include <Windowing/Dialogs/OpenFileDialog.h>
#include <Windowing/Dialogs/SaveFileDialog.h>
#include <Engine/Rendering/ParticleSpriteMaterial.h>
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetPath.h"
#include "Core/Context.h"
#include <Engine/Serialize/ObjectGraphDocument.h>
#include "VFX/Editor/VfxSession.h"
#include "VFX/Editor/VfxStyle.h"
#include "VFX/Editor/VfxSystemPreview.h"
#include "VFX/Editor/VfxInspector.h"
#include "VFX/Editor/VfxStackEditor.h"
#include "VFX/Editor/VfxPreviewCamera.h"
#include "Core/EditorResources.h"
#include "Rendering/EditorGridRenderer.h"
#include "Settings/EditorSettings.h"
#include "VFX/Editor/VfxTimelineView.h"
#include "VFX/Editor/VfxClipboard.h"

namespace NLS::Editor::Panels
{
namespace
{
namespace author = NLS::VFX::Authoring;
namespace ed = ax::NodeEditor;
using VFXTools::VfxSession;
Undo::EditorUndoService* UndoService()
{ return NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>() ? &NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>() : nullptr; }
uint64_t NodeKey(const std::string& id)
{
    uint64_t result = 1469598103934665603ull;
    for (const unsigned char c : id) result = (result^c)*1099511628211ull;
    return (result & ((uint64_t{1}<<60)-1))+1;
}
bool Text(const char* label, std::string& value)
{
    char buffer[1024]{}; std::memcpy(buffer,value.data(),(std::min)(value.size(),sizeof(buffer)-1));
    if (!ImGui::InputText(label,buffer,sizeof(buffer))) return false;
    value = buffer; return true;
}
void Commit(VfxSession& session, const author::System& candidate, bool changed, const char* label)
{
    if (ImGui::IsItemActivated()) session.document.BeginEdit(label);
    if (changed)
    {
        if (session.document.IsEditing()) session.document.PreviewEdit(candidate);
        else session.document.Apply(candidate,label);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() || (ImGui::IsItemDeactivated() && session.document.IsEditing())) session.document.EndEdit();
}
author::Emitter* Selected(author::System& system, const VfxSession& session)
{
    for (auto& emitter : system.emitters) if (emitter.settings.id == session.selectedEmitter) return &emitter;
    return nullptr;
}
const char* InputModeName(author::InputMode mode)
{
    switch (mode)
    {
    case author::InputMode::Literal: return "Value";
    case author::InputMode::Parameter: return "Parameter";
    case author::InputMode::Curve: return "Curve";
    case author::InputMode::DynamicInput: return "Script";
    case author::InputMode::Script: return "Asset";
    }
    return "Value";
}
// The toolbar mirrors NiagaraEditorCommands: compact square icon buttons with
// tooltips. Icons are a merged Font Awesome range; when the host font atlas is
// already locked (headless tests) the buttons keep working as blank targets.
bool IconsReady()
{
    static const bool ready = NLS::UI::Icons::EnsureFontAwesomeIconFontLoaded();
    return ready;
}
std::string IconId(const char* icon,const char* id) { return std::string(icon)+"##"+id; }
bool ToolbarButton(const char* icon,const char* id,const char* tooltip,bool enabled = true)
{
    ImGui::BeginDisabled(!enabled);
    const float size = ImGui::GetFrameHeight();
    const bool pressed = ImGui::Button(IconId(icon,id).c_str(),{size,size});
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tooltip);
    return pressed;
}
bool ToolbarToggle(const char* icon,const char* id,const char* tooltip,bool value,bool enabled = true)
{
    if (value) ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool pressed = ToolbarButton(icon,id,tooltip,enabled);
    if (value) ImGui::PopStyleColor();
    return pressed;
}
void ToolbarDivider()
{
    ImGui::SameLine(0,6);
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine(0,6);
}
void ToolbarStatusText(const char* icon,const ImVec4 color,const char* text,const char* tooltip)
{
    ImGui::SameLine(0,4);
    ImGui::TextColored(color,"%s %s",icon,text);
    if (tooltip && *tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tooltip);
}
}
struct VfxEditor::Document
{
    struct GraphCache
    {
        std::string text;
        std::filesystem::path path;
    };
    static ed::EditorContext* CreateContext(GraphCache& cache)
    {
        ed::Config config; config.SettingsFile = nullptr;
        config.UserPointer = &cache;
        config.SaveSettings = [](const char* data,size_t size,ed::SaveReasonFlags,void* user)
        {
            auto& cache = *static_cast<GraphCache*>(user); cache.text.assign(data,size);
            if (cache.path.empty()) return true;
            std::error_code error; std::filesystem::create_directories(cache.path.parent_path(),error);
            if (error) return false;
            std::ofstream file(cache.path,std::ios::binary|std::ios::trunc); file.write(data,static_cast<std::streamsize>(size)); return static_cast<bool>(file);
        };
        config.LoadSettings = [](char* data,void* user)
        { const auto& text = static_cast<GraphCache*>(user)->text; if (data) std::memcpy(data,text.data(),text.size()); return text.size(); };
        return ed::CreateEditor(&config);
    }
    Document() : session(&undo) { overview = CreateContext(overviewCache); graph = CreateContext(graphCache); }
    ~Document() { ed::SetCurrentEditor(nullptr); ed::DestroyEditor(graph); ed::DestroyEditor(overview); }
    void LoadCache(const std::filesystem::path& root)
    {
        ed::SetCurrentEditor(nullptr); ed::DestroyEditor(overview); ed::DestroyEditor(graph);
        for (auto* cache : {&overviewCache,&graphCache})
        {
            cache->path = root/(session.document.Asset().id+(cache == &overviewCache ? "-overview.json" : "-script.json"));
            std::ifstream file(cache->path,std::ios::binary); cache->text.assign(std::istreambuf_iterator<char>(file),{});
        }
        overview = CreateContext(overviewCache); graph = CreateContext(graphCache);
    }
    Undo::EditorUndoService undo;
    VfxSession session;
    Properties::PropertyPanel properties;
    VFXTools::VfxSystemPreview preview;
    ed::EditorContext *overview = nullptr, *graph = nullptr;
    GraphCache overviewCache, graphCache;
    uint64_t layoutRevision = 0;
    float layoutScale = 0;
    uint64_t graphRevision = 0;
    std::string activeGraph;
    bool graphEditing = false;
    ImGuiID dock = 0;
    bool resetLayout = false, focusScript = false, focusDetails = false, focusCurves = false, isolate = false, showGrid = true, showBounds = false;
    bool showCode = false, showScript = false, showStats = false;
    bool showOriginAxis = true, showParticleCounts = true, focusSearch = false, collapseDisabledModules = false, focusParameters = false;
    bool showPreview = true, showOverview = true, showDetailsPanel = true, showParameters = true, showUserParameters = true;
    bool showLocalModules = true, showTimeline = true, showCurvesPanel = true, showLog = true;
    std::string search, moduleSearch, parameterSearch;
    std::string inspectedSelection;
    VFXTools::VfxModulePicker overviewModules, detailsModules;
    std::unordered_map<std::string,bool> collapsedOverview, collapsedDetails;
    std::vector<std::function<void()>> stackActions;
    std::string revealModule, overviewEmitter;
    VFXTools::VfxPreviewCamera camera;
    std::shared_ptr<const Rendering::EditorGridShaders> gridShaders;
    std::shared_ptr<const Rendering::EditorGridShaders> axesShaders;
    VFXTools::VfxTimelineView timeline;
    VFXTools::VfxCurveView curve;
    bool perspective = true, previewHovered = false;
    float frameRate = 60.0f;
    bool loopRange = true, curveSnap = false, curveMarquee = false;
    uint32_t curveChannels = 0xFu;
    std::string curveSelectionId;
    std::vector<size_t> curveSelection;
    std::array<float,4> curveMarqueeRect{};
    std::string commentEditing;
    std::array<char,256> commentText{};
    bool commentFocus = false;
    float timelineSeek = 0;
    bool* PanelFlag(const std::string& label)
    {
        if (label == "Preview") return &showPreview;
        if (label == "System Overview") return &showOverview;
        if (label == "Details") return &showDetailsPanel;
        if (label == "Parameters") return &showParameters;
        if (label == "User Parameters") return &showUserParameters;
        if (label == "Local Modules") return &showLocalModules;
        if (label == "Timeline") return &showTimeline;
        if (label == "Curves") return &showCurvesPanel;
        if (label == "Niagara Log") return &showLog;
        if (label == "Script Editor") return &showScript;
        if (label == "Generated Code") return &showCode;
        if (label == "Script Stats") return &showStats;
        return nullptr;
    }
    const bool* PanelFlag(const std::string& label) const
    { return const_cast<Document*>(this)->PanelFlag(label); }
};
struct VfxEditor::State
{
    std::filesystem::path cacheRoot;
    std::vector<std::unique_ptr<Document>> documents;
    size_t active = 0;
    std::unique_ptr<NLS::Dialogs::OpenFileDialog> open;
    std::unique_ptr<NLS::Dialogs::SaveFileDialog> save;
    Document *saving = nullptr, *closing = nullptr;
    std::shared_ptr<const Assets::AssetDatabaseFacade> assets;
    std::string error;
    bool keyboardFocus = false;
    author::SourceLibrary library;
    double nextLibraryRefresh = 0;
    struct LibraryRefresh { author::SourceLibrary library; std::string error; };
    std::future<LibraryRefresh> refresh;
};
VfxEditor::VfxEditor(const std::string& title, bool opened, const UI::PanelWindowSettings& settings, std::filesystem::path cacheRoot)
    : PanelWindow(title,opened,settings),m_state(std::make_unique<State>())
{
    m_state->cacheRoot = std::move(cacheRoot); minSize = {1000,700};
    CloseEvent += [this] { for (auto& document : m_state->documents) document->camera.SuspendInputs(); };
}
VfxEditor::~VfxEditor() = default;
bool VfxEditor::OwnsShortcuts() const { return IsOpened() && m_state->keyboardFocus; }
size_t VfxEditor::DocumentCount() const { return m_state->documents.size(); }
void VfxEditor::NewDocument()
{
    m_state->documents.push_back(std::make_unique<Document>()); m_state->active = m_state->documents.size()-1;
    m_state->documents.back()->session.SetLibrary(m_state->library);
    if (!m_state->cacheRoot.empty()) m_state->documents.back()->LoadCache(m_state->cacheRoot);
}
bool VfxEditor::OpenDocument(const std::filesystem::path& path)
{
    for (size_t i = 0; i < m_state->documents.size(); ++i)
        if (m_state->documents[i]->session.document.Path().lexically_normal() == path.lexically_normal())
        { m_state->active = i; Open(); Focus(); return true; }
    auto document = std::make_unique<Document>();
    if (!document->session.document.Load(path))
    { m_state->error = document->session.document.Error(); Open(); Focus(); return false; }
    document->session.SetLibrary(m_state->library);
    if (!m_state->cacheRoot.empty()) document->LoadCache(m_state->cacheRoot);
    m_state->documents.push_back(std::move(document)); m_state->active = m_state->documents.size()-1; Open(); Focus(); return true;
}
void VfxEditor::Save(Document& document)
{
    if (!document.session.document.Path().empty())
    { if (!document.session.document.Save()) m_state->error = document.session.document.Error(); else Publish(document); return; }
        if (!m_state->save)
    {
        m_state->saving = &document;
        m_state->save = std::make_unique<NLS::Dialogs::SaveFileDialog>("Save Particle System","ParticleSystem.vfx",std::vector<std::string>{"Particle System","*.vfx"},true);
    }
}
void VfxEditor::Publish(Document& document)
{
    if (!NLS::Core::ServiceLocator::Contains<Core::Context>()) return;
    const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
    const auto path = std::filesystem::absolute(document.session.document.Path()).lexically_normal();
    const auto assets = std::filesystem::absolute(context.projectAssetsPath).lexically_normal();
    const auto relative = path.lexically_relative(assets);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return;
    const auto roots = Assets::MakeProjectEditorAssetRoots(std::filesystem::path(context.projectPath));
    Assets::AssetDatabaseFacade database(roots);
    if (!database.Refresh() || !database.ImportAsset(Assets::ToEditorAssetPath(roots,path)))
    {
        m_state->error = "The source asset was saved, but its runtime program could not be imported.";
        if (!database.GetDiagnostics().empty()) m_state->error += "\n"+database.GetDiagnostics().back().message;
    }
    // Preview consumes the Asset Browser's next published snapshot. Refreshing
    // another facade here would repeat the complete source scan on the UI lane.
}
void VfxEditor::Dialogs()
{
    if (m_state->open && m_state->open->Ready(0))
    {
        const auto paths = m_state->open->Result(); m_state->open.reset();
        if (!paths.empty()) OpenDocument(paths.front());
    }
    if (m_state->save && m_state->save->Ready(0))
    {
        auto path = std::filesystem::path(m_state->save->Result()); m_state->save.reset();
        if (!path.empty() && m_state->saving)
        {
            if (path.extension().empty()) path += ".vfx";
            if (!m_state->saving->session.document.Save(path)) m_state->error = m_state->saving->session.document.Error();
            else Publish(*m_state->saving);
        }
        m_state->saving = nullptr;
    }
    if (m_state->closing)
    {
        ImGui::OpenPopup("Save Particle System?");
        if (ImGui::BeginPopupModal("Save Particle System?",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Save changes to %s?",m_state->closing->session.document.Asset().name.c_str());
            if (ImGui::Button("Save")) Save(*m_state->closing);
            ImGui::SameLine();
            if (ImGui::Button("Discard") || !m_state->closing->session.document.IsDirty())
            {
                const auto closing = m_state->closing; m_state->closing = nullptr;
                std::erase_if(m_state->documents,[&](const auto& document) { return document.get() == closing; });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button("Cancel")) { m_state->closing = nullptr; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    if (!m_state->error.empty())
    {
        ImGui::OpenPopup("Particle Editor Message");
        if (ImGui::BeginPopupModal("Particle Editor Message",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+550); ImGui::TextUnformatted(m_state->error.c_str()); ImGui::PopTextWrapPos();
            if (ImGui::Button("Close")) { m_state->error.clear(); ImGui::CloseCurrentPopup(); } ImGui::EndPopup();
        }
    }
}
void VfxEditor::Toolbar()
{
    // NiagaraEditorCommands-style grouping: File | History | Compile |
    // Playback | Preview options | Window, with icon buttons and live status.
    if (ToolbarButton(ICON_FA_FILE_O,"File.New","New particle system")) ExecuteCommand("File.New");
    ImGui::SameLine(0,2);
    if (ToolbarButton(ICON_FA_FOLDER_OPEN_O,"File.Open","Open particle system")) ExecuteCommand("File.Open");
    auto* document = ActiveDocument();
    if (!document) return;
    auto& session = document->session;
    ImGui::SameLine(0,2);
    if (ToolbarButton(ICON_FA_FLOPPY_O,"File.Save",document->session.document.IsDirty() ? "Save the current system (unsaved changes)" : "Save the current system")) ExecuteCommand(document,"File.Save");
    ToolbarDivider();
    if (ToolbarButton(ICON_FA_UNDO,"Edit.Undo","Undo")) ExecuteCommand(document,"Edit.Undo");
    ImGui::SameLine(0,2);
    if (ToolbarButton(ICON_FA_REPEAT,"Edit.Redo","Redo")) ExecuteCommand(document,"Edit.Redo");
    ToolbarDivider();
    if (ToolbarButton(ICON_FA_WRENCH,"Compile.Compile","Compile the current system")) ExecuteCommand(document,"Compile");
    ImGui::SameLine(0,2);
    if (ToolbarButton(ICON_FA_CARET_DOWN,"Compile.Options","Compile options"))
    {
        ImGui::OpenPopup("CompileOptions");
    }
    if (ImGui::BeginPopup("CompileOptions"))
    {
        if (ImGui::MenuItem("Auto Compile",nullptr,&session.autoCompile)) {}
        if (ImGui::MenuItem("Full Rebuild")) ExecuteCommand(document,"Compile");
        ImGui::EndPopup();
    }
    ToolbarDivider();
    if (ToolbarButton(ICON_FA_FAST_BACKWARD,"Simulation.Reset","Reset the simulation")) ExecuteCommand(document,"Simulation.Reset");
    ImGui::SameLine(0,2);
    if (ToolbarButton(session.playing ? ICON_FA_PAUSE : ICON_FA_PLAY,"Simulation.PlayPause",session.playing ? "Pause the simulation" : "Play the simulation")) ExecuteCommand(document,"Simulation.PlayPause");
    ImGui::SameLine(0,2);
    if (ToolbarButton(ICON_FA_STEP_FORWARD,"Simulation.Step","Advance one frame")) ExecuteCommand(document,"Simulation.Step");
    ImGui::SameLine(0,2);
    if (ToolbarButton(ICON_FA_SLIDERS,"Simulation.Options","Simulation options"))
    {
        ImGui::OpenPopup("SimulationOptions");
    }
    if (ImGui::BeginPopup("SimulationOptions"))
    {
        ImGui::MenuItem("Auto-play",nullptr,&session.playing);
        ImGui::MenuItem("Reset on change",nullptr,&session.resetOnChange);
        ImGui::MenuItem("Resimulate when paused",nullptr,&session.resimulatePaused);
        ImGui::EndPopup();
    }
    ToolbarDivider();
    if (ToolbarButton(ICON_FA_EXPAND,"Overview.ZoomToFit","Zoom to fit the current selection")) ExecuteCommand(document,"Overview.ZoomToFit");
    ImGui::SameLine(0,2);
    if (ToolbarButton(ICON_FA_PLUS,"Emitter.Add","Add an emitter to the system")) ExecuteCommand(document,"Emitter.Add");
    ToolbarDivider();
    if (ToolbarToggle(ICON_FA_TH,"Preview.Grid","Show the preview grid",document->showGrid)) ExecuteCommand(document,"Preview.Grid");
    ImGui::SameLine(0,2);
    if (ToolbarToggle(ICON_FA_SQUARE_O,"Preview.Bounds","Show the CPU emitter bounds",document->showBounds)) ExecuteCommand(document,"Preview.Bounds");
    ImGui::SameLine(0,2);
    if (ToolbarToggle(ICON_FA_CROSSHAIRS,"Preview.OriginAxis","Show the origin axes",document->showOriginAxis)) ExecuteCommand(document,"Preview.OriginAxis");
    ImGui::SameLine(0,2);
    if (ToolbarToggle(ICON_FA_TABLE,"Preview.ParticleCounts","Show particle counts in the viewport",document->showParticleCounts)) ExecuteCommand(document,"Preview.ParticleCounts");
    ToolbarDivider();
    if (ToolbarButton(ICON_FA_COLUMNS,"Window.Panels","Workspace panels")) ImGui::OpenPopup("WorkspaceWindows");
    if (ImGui::BeginPopup("WorkspaceWindows"))
    {
        struct Entry { const char* label; bool Document::* flag; };
        const Entry entries[] =
        {
            {"Preview",&Document::showPreview},{"System Overview",&Document::showOverview},{"Details",&Document::showDetailsPanel},
            {"Parameters",&Document::showParameters},{"User Parameters",&Document::showUserParameters},{"Local Modules",&Document::showLocalModules},
            {"Timeline",&Document::showTimeline},{"Curves",&Document::showCurvesPanel},{"Niagara Log",&Document::showLog},
            {"Script Editor",&Document::showScript},{"Generated Code",&Document::showCode},{"Script Stats",&Document::showStats}
        };
        for (const auto& entry : entries) ImGui::MenuItem(entry.label,nullptr,&(*document.*entry.flag));
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) ExecuteCommand(document,"View.ResetLayout");
        ImGui::EndPopup();
    }
    // Status feedback: compiled / out of date / failed / not compiled with the
    // first diagnostic on hover and issue navigation on click.
    const auto& issues = session.Issues();
    const char* icon = ICON_FA_CHECK;
    ImVec4 color{0.35f,0.8f,0.45f,1};
    std::string text = "Compiled", tooltip;
    if (!session.Program()) { icon = ICON_FA_INFO_CIRCLE; color = {0.6f,0.6f,0.6f,1}; text = issues.empty() ? "Not compiled" : "Compile failed"; }
    else if (!issues.empty()) { icon = ICON_FA_EXCLAMATION_TRIANGLE; color = {1,0.6f,0.2f,1}; text = std::to_string(issues.size())+" issue(s)"; }
    else if (session.IsOutOfDate()) { icon = ICON_FA_CLOCK_O; color = {0.85f,0.8f,0.35f,1}; text = "Out of date"; }
    if (!issues.empty())
    {
        tooltip = issues.front().message;
        if (!issues.front().input.empty()) tooltip += "\nInput: "+issues.front().input;
    }
    ToolbarStatusText(icon,color,text.c_str(),tooltip.c_str());
    if (!issues.empty() && ImGui::IsItemClicked())
    {
        const auto& issue = issues.front();
        session.document.EndEdit(); session.SelectModule(issue.emitter,issue.stack,issue.module);
        document->focusDetails = true;
    }
}
void VfxEditor::AddEmitter(Document& document)
{
    auto& session = document.session;
    document.properties.EndEdit(); session.document.EndEdit();
    auto candidate = session.document.Asset(); auto emitter = author::CreateEmitter("Emitter "+std::to_string(candidate.emitters.size()+1));
    emitter.position = {float(candidate.emitters.size())*330,0}; const auto id = emitter.settings.id;
    candidate.emitters.push_back(std::move(emitter));
    if (session.document.Apply(std::move(candidate),"Add emitter")) session.SelectEmitter(id);
}
void VfxEditor::CollapseStackToHeaders(Document& document)
{
    document.collapsedDetails.clear(); document.collapsedOverview.clear();
    for (const auto& emitter : document.session.document.Asset().emitters)
        for (const auto& stack : emitter.stacks)
        {
            const auto key = emitter.settings.id+":"+std::string(VFXTools::VfxSourceId(stack));
            document.collapsedDetails[key] = false; document.collapsedOverview[key] = false;
            for (const auto& module : stack.modules) { document.collapsedDetails[module.id] = true; document.collapsedOverview[module.id] = true; }
        }
}
VfxEditor::Document* VfxEditor::ActiveDocument()
{
    if (m_state->documents.empty()) return nullptr;
    m_state->active = (std::min)(m_state->active,m_state->documents.size()-1);
    return m_state->documents[m_state->active].get();
}
const VfxEditor::Document* VfxEditor::ActiveDocument() const
{
    if (m_state->documents.empty()) return nullptr;
    return m_state->documents[(std::min)(m_state->active,m_state->documents.size()-1)].get();
}
bool VfxEditor::ExecuteCommand(const std::string& id) { return ExecuteCommand(ActiveDocument(),id); }
bool VfxEditor::IsCommandEnabled(const std::string& id) const { return IsCommandEnabled(ActiveDocument(),id); }
bool VfxEditor::IsCommandChecked(const std::string& id) const { return IsCommandChecked(ActiveDocument(),id); }
size_t VfxEditor::ActiveEmitterCount() const
{
    const auto* document = ActiveDocument();
    return document ? document->session.document.Asset().emitters.size() : 0;
}
bool VfxEditor::SetWorkspacePanel(const std::string& label,bool visible)
{
    auto* document = ActiveDocument(); if (!document) return false;
    bool* flag = document->PanelFlag(label); if (!flag) return false;
    *flag = visible; return true;
}
bool VfxEditor::IsWorkspacePanelVisible(const std::string& label) const
{
    const auto* document = ActiveDocument(); if (!document) return false;
    const bool* flag = document->PanelFlag(label); return flag && *flag;
}
bool VfxEditor::IsCommandEnabled(const Document* document,const std::string& id) const
{
    if (id == "File.New" || id == "File.Open") return true;
    if (!document) return false;
    const auto& session = document->session;
    if (id == "Edit.Undo") return document->undo.CanUndo();
    if (id == "Edit.Redo") return document->undo.CanRedo();
    if (id == "Edit.Copy" || id == "Edit.Cut") return !session.SelectedModules().empty() || !session.SelectedEmitters().empty();
    if (id == "Edit.Paste") return !VFXTools::VfxClipboard::Get().Empty();
    if (id == "Simulation.Reset" || id == "Simulation.PlayPause" || id == "Simulation.Step" || id == "Simulation.StepBack") return session.Program() != nullptr;
    if (id == "Select.Isolate") return !session.selectedEmitter.empty() || session.selection == VFXTools::VfxSelection::System;
    if (id == "Select.DisableEmitters" || id == "Select.EnableEmitters") return !session.selectedEmitter.empty();
    return true;
}
bool VfxEditor::IsCommandChecked(const Document* document,const std::string& id) const
{
    if (!document) return false;
    const auto& session = document->session;
    if (id == "Compile.AutoCompile") return session.autoCompile;
    if (id == "Preview.Grid") return document->showGrid;
    if (id == "Preview.Bounds") return document->showBounds;
    if (id == "Preview.OriginAxis") return document->showOriginAxis;
    if (id == "Preview.ParticleCounts") return document->showParticleCounts;
    if (id == "Preview.ToggleProjection") return document->perspective;
    if (id == "Select.Isolate") return document->isolate;
    if (id == "Stack.CollapseDisabled") return document->collapseDisabledModules;
    if (id == "Simulation.PlayPause") return session.playing;
    return false;
}
bool VfxEditor::ExecuteCommand(Document* document,const std::string& id)
{
    if (!IsCommandEnabled(document,id)) return false;
    if (id == "File.New") { NewDocument(); return true; }
    if (id == "File.Open")
    {
        if (!m_state->open) m_state->open = std::make_unique<NLS::Dialogs::OpenFileDialog>("Open Particle System","",std::vector<std::string>{"Particle System","*.vfx"});
        return true;
    }
    if (!document) return false;
    auto& session = document->session;
    if (id == "File.Save") { Save(*document); return true; }
    if (id == "Edit.Undo") { session.document.EndEdit(); (void)document->undo.RequestUndo(); return true; }
    if (id == "Edit.Redo") { session.document.EndEdit(); (void)document->undo.RequestRedo(); return true; }
    if (id == "Edit.Copy")
    {
        const auto candidate = session.document.Asset();
        if (!session.SelectedModules().empty()) VFXTools::VfxClipboard::Get().CopyModules(candidate,session.SelectedModules());
        else if (!session.SelectedEmitters().empty()) VFXTools::VfxClipboard::Get().CopyEmitters(candidate,session.SelectedEmitters());
        else return false;
        return true;
    }
    if (id == "Edit.Cut")
    {
        const auto candidate = session.document.Asset();
        const auto modules = session.SelectedModules(), emitters = session.SelectedEmitters();
        document->properties.EndEdit(); session.document.EndEdit();
        if (!modules.empty())
        {
            VFXTools::VfxClipboard::Get().CopyModules(candidate,modules);
            return session.DeleteModules(modules);
        }
        if (!emitters.empty())
        {
            VFXTools::VfxClipboard::Get().CopyEmitters(candidate,emitters);
            return session.DeleteEmitters(emitters);
        }
        return false;
    }
    if (id == "Edit.Paste")
    {
        auto candidate = session.document.Asset(); std::string error;
        auto& clipboard = VFXTools::VfxClipboard::Get();
        // Modules paste into the primary module's stage, falling back to the
        // selected emitter's first local stage.
        std::string emitterId = session.selectedEmitter, stackId;
        const auto locateDestination = [&]()
        {
            for (auto& emitter : candidate.emitters) for (auto& stack : emitter.stacks)
            {
                if (!stack.inheritedId.empty()) continue;
                if (!session.selectedModule.empty() && std::any_of(stack.modules.begin(),stack.modules.end(),
                    [&](const auto& module) { return module.id == session.selectedModule; }))
                { emitterId = emitter.settings.id; stackId = stack.id; return true; }
            }
            for (auto& emitter : candidate.emitters) if (emitter.settings.id == emitterId)
                for (auto& stack : emitter.stacks) if (stack.inheritedId.empty()) { stackId = stack.id; return true; }
            return false;
        };
        if (clipboard.HasModules())
        {
            if (!locateDestination()) { m_state->error = "Select a local stage before pasting modules"; return false; }
            std::vector<std::string> newIds;
            if (!clipboard.PasteModules(candidate,emitterId,stackId,newIds,error)) { m_state->error = error; return false; }
            document->properties.EndEdit(); session.document.EndEdit();
            if (!session.document.Apply(std::move(candidate),"Paste modules")) { m_state->error = session.document.Error(); return false; }
            session.SelectModules(newIds);
            return true;
        }
        if (clipboard.HasEmitters())
        {
            std::vector<std::string> newIds;
            if (!clipboard.PasteEmitters(candidate,newIds,error)) { m_state->error = error; return false; }
            document->properties.EndEdit(); session.document.EndEdit();
            if (!session.document.Apply(std::move(candidate),"Paste emitters")) { m_state->error = session.document.Error(); return false; }
            session.SelectEmitter(newIds.front());
            return true;
        }
        m_state->error = "The clipboard is empty";
        return false;
    }
    if (id == "Compile") { session.Rebuild(true); return true; }
    if (id == "Compile.AutoCompile") { session.autoCompile = !session.autoCompile; return true; }
    if (id == "Simulation.Reset") { session.Reset(); return true; }
    if (id == "Simulation.PlayPause") { session.playing = !session.playing; return true; }
    if (id == "Simulation.Step") { session.Advance(1.0/static_cast<double>((std::max)(1.0f,document->frameRate))); return true; }
    if (id == "Simulation.StepBack")
    {
        const auto age = session.Instance().Age();
        session.RequestSeek((std::max)(0.0,age-1.0/static_cast<double>((std::max)(1.0f,document->frameRate))));
        return true;
    }
    if (id == "Preview.Grid") { document->showGrid = !document->showGrid; return true; }
    if (id == "Preview.Bounds") { document->showBounds = !document->showBounds; return true; }
    if (id == "Preview.OriginAxis") { document->showOriginAxis = !document->showOriginAxis; return true; }
    if (id == "Preview.ParticleCounts") { document->showParticleCounts = !document->showParticleCounts; return true; }
    if (id == "Preview.ToggleProjection") { document->perspective = !document->perspective; document->camera.Camera().SetProjectionMode(document->perspective ? Render::Settings::EProjectionMode::PERSPECTIVE : Render::Settings::EProjectionMode::ORTHOGRAPHIC); return true; }
    if (id == "Preview.FocusSelection") { FocusPreview(*document,true); return true; }
    if (id == "Preview.FocusAll") { FocusPreview(*document,false); return true; }
    if (id == "Select.Isolate") { document->isolate = !document->isolate; return true; }
    if (id == "Select.DisableEmitters" || id == "Select.EnableEmitters")
    {
        const bool enabled = id == "Select.EnableEmitters";
        auto candidate = session.document.Asset();
        if (auto* emitter = Selected(candidate,session))
        {
            emitter->enabled = enabled;
            document->properties.EndEdit(); session.document.EndEdit();
            session.document.Apply(std::move(candidate),enabled ? "Enable emitter" : "Disable emitter");
        }
        return true;
    }
    if (id == "Overview.ZoomToFit")
    {
        ed::SetCurrentEditor(document->overview); ed::NavigateToSelection(); ed::SetCurrentEditor(nullptr); return true;
    }
    if (id == "Overview.ZoomToFitAll")
    {
        ed::SetCurrentEditor(document->overview); ed::NavigateToContent(); ed::SetCurrentEditor(nullptr); return true;
    }
    if (id == "Stack.CollapseToHeaders") { CollapseStackToHeaders(*document); return true; }
    if (id == "Stack.CollapseDisabled") { document->collapseDisabledModules = !document->collapseDisabledModules; return true; }
    if (id == "Emitter.Add") { AddEmitter(*document); return true; }
    if (id == "Module.Delete")
    {
        if (session.selectedModule.empty()) return false;
        auto candidate = session.document.Asset(); bool removed = false;
        for (auto& emitter : candidate.emitters) for (auto& stack : emitter.stacks)
            removed |= std::erase_if(stack.modules,[&](const auto& module) { return module.id == session.selectedModule; }) > 0;
        if (removed) { document->properties.EndEdit(); session.document.EndEdit(); session.document.Apply(std::move(candidate),"Delete module"); session.ReconcileSelection(); }
        return removed;
    }
    if (id == "View.ResetLayout")
    {
        document->resetLayout = true;
        document->showPreview = document->showOverview = document->showDetailsPanel = true;
        document->showParameters = document->showUserParameters = document->showLocalModules = true;
        document->showTimeline = document->showCurvesPanel = document->showLog = true;
        document->showScript = document->showCode = document->showStats = false;
        return true;
    }
    return false;
}
void VfxEditor::Shortcuts(Document& document)
{
    // Key bindings mirror FNiagaraEditorCommands (5.7.4): E Add Emitter,
    // Space Reset, S Isolate, D / Shift+D Disable/Enable emitters,
    // F Zoom to Fit, A Zoom to Fit All, Ctrl+O Collapse to Headers,
    // Ctrl+M Collapse disabled modules, Ctrl+F Find, Del remove module.
    if (!m_state->keyboardFocus || ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)) return;
    // Navigation owns its keys until the shared camera controller releases capture.
    if (document.camera.IsNavigating()) return;
    auto& session = document.session; const auto& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S,false)) ExecuteCommand(&document,"File.Save");
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z,false)) ExecuteCommand(&document,"Edit.Undo");
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y,false)) ExecuteCommand(&document,"Edit.Redo");
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C,false)) ExecuteCommand(&document,"Edit.Copy");
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X,false)) ExecuteCommand(&document,"Edit.Cut");
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V,false)) ExecuteCommand(&document,"Edit.Paste");
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O,false)) ExecuteCommand(&document,"Stack.CollapseToHeaders");
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M,false)) ExecuteCommand(&document,"Stack.CollapseDisabled");
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F,false)) document.focusSearch = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Space,false)) ExecuteCommand(&document,"Simulation.Reset");
    if (ImGui::IsKeyPressed(ImGuiKey_Escape,false) && session.document.IsEditing()) session.document.EndEdit(false);
    if (io.KeyCtrl || io.KeyAlt) return;
    if (ImGui::IsKeyPressed(ImGuiKey_S,false)) ExecuteCommand(&document,"Select.Isolate");
    if (ImGui::IsKeyPressed(ImGuiKey_D,false)) ExecuteCommand(&document,io.KeyShift ? "Select.EnableEmitters" : "Select.DisableEmitters");
    if (ImGui::IsKeyPressed(ImGuiKey_F,false)) ExecuteCommand(&document,document.previewHovered ? "Preview.FocusSelection" : "Overview.ZoomToFit");
    if (ImGui::IsKeyPressed(ImGuiKey_A,false)) ExecuteCommand(&document,"Overview.ZoomToFitAll");
    if (ImGui::IsKeyPressed(ImGuiKey_E,false)) ExecuteCommand(&document,"Emitter.Add");
    if (ImGui::IsKeyPressed(ImGuiKey_Delete,false)) ExecuteCommand(&document,"Module.Delete");
}
void VfxEditor::OnBeforeDrawWidgets()
{
    VFXTools::VfxStyle style;
    m_state->keyboardFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    Dialogs(); if (m_state->documents.empty()) NewDocument();
    if (m_state->refresh.valid() && m_state->refresh.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        auto result = m_state->refresh.get();
        if (result.error.empty())
        { m_state->library = std::move(result.library); for (auto& document : m_state->documents) document->session.SetLibrary(m_state->library); }
        else for (auto& document : m_state->documents) document->session.SetLibraryError(result.error);
    }
    if (!m_state->refresh.valid() && ImGui::GetTime() >= m_state->nextLibraryRefresh && NLS::Core::ServiceLocator::Contains<Core::Context>())
    {
        m_state->nextLibraryRefresh = ImGui::GetTime()+2;
        const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
        const std::array roots{std::filesystem::path(context.projectAssetsPath),std::filesystem::path(context.engineAssetsPath)};
        m_state->refresh = std::async(std::launch::async,[roots] { State::LibraryRefresh result; result.library.Refresh(roots,result.error); return result; });
    }
    m_state->active = (std::min)(m_state->active,m_state->documents.size()-1);
    Toolbar(); ImGui::Separator();
    if (ImGui::BeginTabBar("VfxDocuments",ImGuiTabBarFlags_Reorderable|ImGuiTabBarFlags_AutoSelectNewTabs))
    {
        for (size_t i = 0; i < m_state->documents.size(); ++i)
        {
            auto& document = *m_state->documents[i]; auto& session = document.session; bool opened = true;
            const auto title = session.document.Asset().name+"###"+session.document.Asset().id;
            const auto flags = session.document.IsDirty() ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(title.c_str(),&opened,flags))
            {
                m_state->active = i; session.Rebuild(); Workspace(document); Shortcuts(document); ImGui::EndTabItem();
            }
            else document.camera.SuspendInputs();
            if (!opened)
            {
                if (session.document.IsDirty()) m_state->closing = &document;
                else if (m_state->saving != &document) { m_state->documents.erase(m_state->documents.begin()+i); --i; }
            }
        }
        ImGui::EndTabBar();
    }
}
void VfxEditor::Workspace(Document& document)
{
    auto& session = document.session;
    document.properties.SetLifetime(session.document.Lifetime());
    const auto selection = session.selectedEmitter+":"+session.selectedStack+":"+session.selectedModule;
    if (document.inspectedSelection != selection)
    {
        document.properties.EndEdit(); session.document.EndEdit(); document.inspectedSelection = selection;
    }
    if (const auto* undo = UndoService())
    {
        if (undo->IsFrozen()) document.undo.Freeze(Undo::UndoFreezeReason::PlayMode);
        else document.undo.Unfreeze(Undo::UndoFreezeReason::PlayMode);
    }
    const auto suffix = "###VfxEditor:"+session.document.Asset().id+":";
    const auto title = [&](const char* label) { return std::string(label)+suffix+label; };
    document.dock = ImGui::GetID(("Dock:"+session.document.Asset().id).c_str());
    ImGuiWindowClass windowClass; windowClass.ClassId = document.dock; windowClass.DockingAllowUnclassed = false;
    if (document.resetLayout || !ImGui::DockBuilderGetNode(document.dock))
    {
        document.resetLayout = false; ImGui::DockBuilderRemoveNode(document.dock);
        ImGui::DockBuilderAddNode(document.dock,ImGuiDockNodeFlags_DockSpace); ImGui::DockBuilderSetNodeSize(document.dock,ImGui::GetContentRegionAvail());
        ImGuiID center = document.dock;
        auto left = ImGui::DockBuilderSplitNode(center,ImGuiDir_Left,0.33f,nullptr,&center);
        auto right = ImGui::DockBuilderSplitNode(center,ImGuiDir_Right,0.5f,nullptr,&center);
        auto leftBottom = ImGui::DockBuilderSplitNode(left,ImGuiDir_Down,0.5f,nullptr,&left);
        auto bottom = ImGui::DockBuilderSplitNode(center,ImGuiDir_Down,0.33f,nullptr,&center);
        for (const auto* label : {"Preview"}) ImGui::DockBuilderDockWindow(title(label).c_str(),left);
        for (const auto* label : {"Parameters","User Parameters","Local Modules"}) ImGui::DockBuilderDockWindow(title(label).c_str(),leftBottom);
        for (const auto* label : {"System Overview","Script Editor"}) ImGui::DockBuilderDockWindow(title(label).c_str(),center);
        for (const auto* label : {"Timeline","Curves","Niagara Log","Script Stats"}) ImGui::DockBuilderDockWindow(title(label).c_str(),bottom);
        for (const auto* label : {"Details","Generated Code"}) ImGui::DockBuilderDockWindow(title(label).c_str(),right);
        ImGui::DockBuilderFinish(document.dock);
    }
    ImGui::DockSpace(document.dock,{0,0},ImGuiDockNodeFlags_None,&windowClass);
    const auto draw = [&](const char* label, auto callback)
    {
        ImGui::SetNextWindowClass(&windowClass);
        if ((std::string_view(label) == "Details" && std::exchange(document.focusDetails,false)) ||
            (std::string_view(label) == "Script Editor" && std::exchange(document.focusScript,false)) ||
            (std::string_view(label) == "Parameters" && std::exchange(document.focusParameters,false)) ||
            (std::string_view(label) == "Curves" && std::exchange(document.focusCurves,false))) ImGui::SetNextWindowFocus();
        if (ImGui::Begin(title(label).c_str()))
        {
            m_state->keyboardFocus |= ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            const bool editable = std::string_view(label) == "System Overview" || std::string_view(label) == "Details" || std::string_view(label) == "Parameters" || std::string_view(label) == "Script Editor" || std::string_view(label) == "Curves";
            if (editable) ImGui::BeginDisabled(document.undo.IsFrozen() || document.undo.IsPending() || document.undo.IsPoisoned());
            callback();
            if (editable) ImGui::EndDisabled();
        }
        else if (std::string_view(label) == "Preview") document.camera.SuspendInputs();
        ImGui::End();
    };
    // Tick only when the render publication queue can accept another frame.
    // In a headless editor CPU authoring remains usable without a GPU preview.
    if (document.showPreview) draw("Preview",[&] { Preview(document); });
    else document.camera.SuspendInputs();
    if (document.showOverview) draw("System Overview",[&] { Overview(document); });
    if (document.showDetailsPanel) draw("Details",[&] { Details(document); });
    if (document.showParameters) draw("Parameters",[&] { Parameters(document,false); });
    if (document.showUserParameters) draw("User Parameters",[&] { Parameters(document,true); });
    if (document.showLocalModules) draw("Local Modules",[&]
    {
        for (const auto& emitter : session.document.Asset().emitters) for (const auto& stack : emitter.stacks) for (const auto& module : stack.modules)
            for (const auto& [name,input] : module.inputs) if (input.mode == author::InputMode::DynamicInput)
            {
                ImGui::PushID(module.id.c_str());
                if (ImGui::Selectable((module.script+" / "+name).c_str(),session.selectedModule == module.id))
                { session.SelectModule(emitter.settings.id,stack.id,module.id); document.showScript = true; }
                ImGui::PopID();
            }
    });
    if (document.showTimeline) draw("Timeline",[&] { Timeline(document); });
    if (document.showCurvesPanel) draw("Curves",[&] { Curves(document); });
    if (document.showLog) draw("Niagara Log",[&] { Diagnostics(document); });
    if (document.showScript) draw("Script Editor",[&] { Script(document); });
    if (document.showCode) draw("Generated Code",[&]
    {
        if (session.Program()) for (size_t i = 0; i < session.Program()->emitters.size(); ++i)
            if (session.Program()->asset.emitters[i].id == session.selectedEmitter)
            {
                const auto& program = session.Program()->emitters[i]; const auto& emitter = session.Program()->asset.emitters[i];
                ImGui::SeparatorText("Particle Spawn"); ImGui::TextUnformatted(program.spawnHlsl.c_str());
                ImGui::SeparatorText("Particle Update"); ImGui::TextUnformatted(program.updateHlsl.c_str());
                for (size_t stage = 0; stage < program.stages.size(); ++stage)
                { ImGui::SeparatorText(emitter.simulationStages[stage].name.c_str()); ImGui::TextUnformatted(program.stages[stage].hlsl.c_str()); }
            }
    });
    if (document.showStats) draw("Script Stats",[&]
    {
        if (session.Program()) for (size_t i = 0; i < session.Program()->emitters.size(); ++i)
        {
            const auto& program = session.Program()->emitters[i]; const auto& emitter = session.Program()->asset.emitters[i];
            ImGui::Text("%s: Spawn %zu / Update %zu instructions",emitter.name.c_str(),program.spawn.instructions.size(),program.update.instructions.size());
            std::vector<NLS::VFX::SimulationStageExecution> controls; std::string controlError;
            const auto frame = session.Instance().Capture();
            if (frame) NLS::VFX::ResolveSimulationStageControls(emitter.simulationStages,frame->parameters,controls,controlError);
            for (size_t stage = 0; stage < program.stages.size(); ++stage)
            {
                const auto& settings = emitter.simulationStages[stage];
                const auto count = stage < controls.size() ? controls[stage].iterations : settings.iterations;
                const bool enabled = stage < controls.size() ? controls[stage].enabled : settings.enabled;
                ImGui::Text("  %s: %zu instructions x%u%s",settings.name.c_str(),program.stages[stage].program.instructions.size(),count,enabled && count ? "" : " (disabled)");
            }
        }
    });
    if (document.properties.FinishFrame()) session.document.EndEdit();
    auto actions = std::exchange(document.stackActions,{});
    if (!actions.empty()) { document.properties.EndEdit(); session.document.EndEdit(); }
    for (auto& action : actions) action();
    document.focusSearch = false;
    session.ReconcileSelection();
}

void VfxEditor::Overview(Document& document)
{
    auto& session = document.session;
    if (ImGui::Button("+ Emitter")) AddEmitter(document);
    ImGui::SameLine();
    if (ImGui::Button("Add Emitter Asset")) ImGui::OpenPopup("EmitterAssets");
    if (ImGui::BeginPopup("EmitterAssets"))
    {
        for (const auto& [id,source] : session.Library().Emitters()) if (ImGui::MenuItem(source.name.c_str()))
        {
            document.properties.EndEdit(); session.document.EndEdit();
            auto candidate = session.document.Asset(); auto emitter = author::InstantiateEmitter(source);
            emitter.position = {float(candidate.emitters.size())*330,0}; const auto emitterId = emitter.settings.id;
            candidate.emitters.push_back(std::move(emitter));
            if (session.document.Apply(std::move(candidate),"Add referenced emitter")) session.SelectEmitter(emitterId);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(); if (ImGui::Button("Zoom to Fit")) { ed::SetCurrentEditor(document.overview); ed::NavigateToContent(); ed::SetCurrentEditor(nullptr); }
    // Display the working authoring model even while a compile is failing. A
    // last-good runtime program must not put deleted or disabled rows back.
    const auto asset = session.EditingSystem();
    bool rowSelected = false;
    const float layoutScale = ImGui::GetFontSize()/13.0f;
    const float width = 250.0f*layoutScale;
    ed::SetCurrentEditor(document.overview);
    ed::PushStyleVar(ed::StyleVar_NodePadding,ImVec4{1,1,1,1});
    ed::PushStyleVar(ed::StyleVar_NodeRounding,3.0f);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth,1.0f);
    ed::PushStyleVar(ed::StyleVar_HoveredNodeBorderWidth,1.0f);
    ed::PushStyleVar(ed::StyleVar_SelectedNodeBorderWidth,1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{0,0});
    ed::Begin("SystemOverview",{0,0});
    if (document.layoutRevision != session.document.Revision() || document.layoutScale != layoutScale)
    {
        ed::SetNodePosition(ed::NodeId(NodeKey(asset.id)),{-330*layoutScale,0});
        for (const auto& emitter : asset.emitters) ed::SetNodePosition(ed::NodeId(NodeKey(emitter.settings.id)),{emitter.position[0]*layoutScale,emitter.position[1]*layoutScale});
        document.layoutRevision = session.document.Revision(); document.layoutScale = layoutScale;
    }
    ed::BeginNode(ed::NodeId(NodeKey(asset.id)));
    ImGui::PushID(asset.id.c_str());
    VFXTools::VfxStackRow systemRow; systemRow.label = asset.name.c_str(); systemRow.width = width;
    systemRow.header = true; systemRow.title = true; systemRow.selected = session.selection == VFXTools::VfxSelection::System;
    systemRow.accent = VFXTools::VfxStyle::StageColor(author::Stage::SystemUpdate);
    if (VFXTools::DrawVfxStackRow(systemRow).selected) { rowSelected = true; document.properties.EndEdit(); session.SelectSystem(); }
    if (ImGui::BeginPopupContextItem("SystemNodeActions"))
    {
        if (ImGui::MenuItem("Add Comment"))
        {
            auto value = session.document.Asset(); author::Comment comment;
            comment.id = Guid::New().ToString(); comment.text = "Comment";
            const auto canvas = ed::ScreenToCanvas({ImGui::GetWindowPos().x+320.0f,ImGui::GetWindowPos().y+220.0f});
            comment.position = {canvas.x,canvas.y};
            value.comments.push_back(std::move(comment));
            session.document.Apply(std::move(value),"Add comment");
        }
        if (ImGui::MenuItem("Group Selected Emitters",nullptr,false,!session.SelectedEmitters().empty()))
        {
            auto value = session.document.Asset(); author::Group group;
            group.id = Guid::New().ToString(); group.title = "Group "+std::to_string(value.groups.size()+1);
            group.emitters = session.SelectedEmitters();
            value.groups.push_back(std::move(group));
            session.document.Apply(std::move(value),"Add group");
        }
        ImGui::EndPopup();
    }
    if (ImGui::Selectable("  Properties")) { rowSelected = true; document.properties.EndEdit(); session.SelectSystem(); document.focusDetails = true; }
    const auto parameterLabel = "  User Parameters ("+std::to_string(asset.parameters.size())+")";
    if (ImGui::Selectable(parameterLabel.c_str())) { rowSelected = true; document.properties.EndEdit(); session.SelectSystem(); document.focusParameters = true; }
    const auto emitterLabel = "  Emitters ("+std::to_string(asset.emitters.size())+")";
    ImGui::TextDisabled("%s",emitterLabel.c_str());
    ImGui::Separator(); ImGui::TextDisabled("  System Spawn"); ImGui::TextDisabled("  System Update");
    ImGui::Dummy({width,2}); ImGui::PopID(); ed::EndNode();
    for (const auto& emitter : asset.emitters)
    {
        const auto emitterId = emitter.settings.id;
        ImGui::PushID(emitterId.c_str());
        ed::PushStyleColor(ed::StyleColor_NodeBg,{0.075f,0.075f,0.075f,1});
        ed::PushStyleColor(ed::StyleColor_NodeBorder,{0.015f,0.015f,0.015f,1});
        ed::BeginNode(ed::NodeId(NodeKey(emitterId)));
        VFXTools::VfxStackRow header; header.label = emitter.settings.name.c_str(); header.width = width;
        header.header = true; header.title = true; header.enable = true; header.enabled = emitter.enabled;
        header.expandable = true; header.expanded = !document.collapsedOverview[emitterId];
        header.selected = session.IsEmitterSelected(emitterId) && session.selection != VFXTools::VfxSelection::System;
        header.accent = VFXTools::VfxStyle::StageColor(author::Stage::EmitterUpdate);
        const auto* headerRenderer = emitter.settings.renderer == NLS::VFX::RendererType::Sprite ? "Sprite Renderer" : emitter.settings.renderer == NLS::VFX::RendererType::Mesh ? "Mesh Renderer" : "Ribbon Renderer";
        const std::string emitterSummary = std::string(emitter.settings.target == NLS::VFX::SimulationTarget::CPU ? "CPU" : "GPU")+"  "+headerRenderer;
        header.summary = emitterSummary.c_str();
        const auto emitterRow = VFXTools::DrawVfxStackRow(header);
        if (emitterRow.selected) { rowSelected = true; document.properties.EndEdit(); session.SelectEmitter(emitterId,ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift); }
        if (emitterRow.expanded) document.collapsedOverview[emitterId] = !document.collapsedOverview[emitterId];
        if (emitterRow.enabled) document.stackActions.push_back([this,&document,emitterId,enabled = !emitter.enabled]
        { if (!document.session.SetEmitterEnabled(emitterId,enabled)) m_state->error = document.session.ActionError(); });
        if (ImGui::BeginPopupContextItem("EmitterNodeActions"))
        {
            if (ImGui::MenuItem("Copy","Ctrl+C")) ExecuteCommand(&document,"Edit.Copy");
            if (ImGui::MenuItem("Paste","Ctrl+V",false,!VFXTools::VfxClipboard::Get().Empty())) ExecuteCommand(&document,"Edit.Paste");
            ImGui::Separator();
            const auto emitters = session.SelectedEmitters();
            const auto count = std::to_string((std::max)(size_t{1},emitters.size()));
            if (session.IsEmitterSelected(emitterId) && emitters.size() > 1)
            {
                if (ImGui::MenuItem(("Enable "+count+" selected").c_str())) document.stackActions.push_back([&document,emitters]
                { document.properties.EndEdit(); document.session.document.EndEdit(); document.session.SetEmittersEnabled(emitters,true); });
                if (ImGui::MenuItem(("Disable "+count+" selected").c_str())) document.stackActions.push_back([&document,emitters]
                { document.properties.EndEdit(); document.session.document.EndEdit(); document.session.SetEmittersEnabled(emitters,false); });
                if (ImGui::MenuItem(("Delete "+count+" selected").c_str())) document.stackActions.push_back([&document,emitters]
                { document.properties.EndEdit(); document.session.document.EndEdit(); document.session.DeleteEmitters(emitters); });
            }
            else
            {
                if (ImGui::MenuItem(emitter.enabled ? "Disable emitter" : "Enable emitter")) document.stackActions.push_back([this,&document,emitterId,enabled = !emitter.enabled]
                { if (!document.session.SetEmitterEnabled(emitterId,enabled)) m_state->error = document.session.ActionError(); });
                if (ImGui::MenuItem("Delete emitter")) document.stackActions.push_back([&document,emitterId]
                { auto value = document.session.document.Asset(); std::erase_if(value.emitters,[&](const auto& item) { return item.settings.id == emitterId; }); document.session.document.Apply(std::move(value),"Delete emitter"); });
            }
            if (ImGui::MenuItem("Isolate selected emitters")) document.isolate = true;
            if (ImGui::MenuItem("Stop isolating")) document.isolate = false;
            ImGui::EndPopup();
        }
        if (!document.collapsedOverview[emitterId])
        {
            for (const auto& stack : emitter.stacks)
            {
                const auto stackId = std::string(VFXTools::VfxSourceId(stack));
                const auto key = emitterId+":"+stackId;
                const bool localStage = stack.inheritedId.empty();
                ImGui::PushID(stackId.c_str());
                VFXTools::VfxStackRow stage; stage.label = stack.stage == author::Stage::Simulation ? stack.name.c_str() : author::StageName(stack.stage);
                const std::string stageSummary = std::to_string(stack.modules.size())+(stack.modules.size() == 1 ? " module" : " modules")+
                    (stack.stage == author::Stage::Simulation && stack.iterations > 1 ? "  x"+std::to_string(stack.iterations) : std::string{});
                stage.summary = stageSummary.c_str();
                stage.width = width; stage.header = true; stage.expandable = true; stage.add = true;
                stage.expanded = !document.collapsedOverview[key]; stage.enabled = stack.enabled;
                stage.selected = session.selectedEmitter == emitterId && session.selectedStack == stackId && session.selection == VFXTools::VfxSelection::Stack;
                stage.accent = VFXTools::VfxStyle::StageIconColor(stack.stage);
                const auto stageRow = VFXTools::DrawVfxStackRow(stage);
                if (stageRow.expanded) document.collapsedOverview[key] = !document.collapsedOverview[key];
                if (stageRow.selected) { rowSelected = true; document.properties.EndEdit(); session.SelectStack(emitterId,stackId); }
                if (stageRow.add)
                { rowSelected = true; document.properties.EndEdit(); session.SelectStack(emitterId,stackId); document.overviewModules.Open(emitterId,stackId); }
                if (localStage && ImGui::BeginDragDropTarget())
                {
                    if (const auto* payload = ImGui::AcceptDragDropPayload("NLS_VFX_MODULE"))
                        document.stackActions.push_back([this,&document,from = std::string(static_cast<const char*>(payload->Data)),to = stack.id,at = stack.modules.size()]
                        { auto value = document.session.document.Asset(); std::string error; if (author::MoveModule(value,from,to,at,error,&document.session.Library())) document.session.document.Apply(std::move(value),"Move module"); else m_state->error = error; });
                    ImGui::EndDragDropTarget();
                }
                if (!document.collapsedOverview[key]) for (size_t moduleIndex = 0; moduleIndex < stack.modules.size(); ++moduleIndex)
                {
                    const auto& module = stack.modules[moduleIndex];
                    const auto moduleId = std::string(VFXTools::VfxSourceId(module));
                    const bool localModule = module.inheritedId.empty();
                    const auto* definition = author::FindModule(module.script);
                    const auto* script = session.Library().Script(module.scriptAsset.asset);
                    author::ModuleDefinition externalDefinition;
                    if (script) if (const auto* version = author::SelectVersion(*script,module.scriptAsset.version))
                    {
                        externalDefinition.id = script->id; externalDefinition.name = script->name; externalDefinition.stage = stack.stage;
                        for (const auto& input : version->inputs) externalDefinition.inputs.push_back({input.name,input.type,input.defaultValue,input.metadata});
                        definition = &externalDefinition;
                    }
                    const auto& name = script ? script->name : definition ? definition->name : module.script;
                    const auto summary = VFXTools::VfxInputSummary(module,definition);
                    ImGui::PushID(moduleId.c_str());
                    VFXTools::VfxStackRow row; row.label = name.c_str(); row.width = width; row.enable = true; row.enabled = module.enabled;
                    row.summary = summary.c_str();
                    row.muted = !emitter.enabled || !stack.enabled;
                    row.accent = stage.accent; row.inherited = !module.inheritedId.empty();
                    row.selected = session.IsModuleSelected(moduleId);
                    row.remove = localModule;
                    row.issue = std::any_of(session.Issues().begin(),session.Issues().end(),[&](const auto& issue)
                    { return issue.emitter == emitterId && (issue.module == moduleId || issue.module == module.id); });
                    const auto moduleRow = VFXTools::DrawVfxStackRow(row);
                    if (moduleRow.selected || moduleRow.doubleClicked)
                    {
                        rowSelected = true;
                        document.properties.EndEdit(); session.SelectModule(emitterId,stackId,moduleId,ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
                        if (moduleRow.doubleClicked && OpenModuleSource(document,module))
                        {
                            // UE opens the module script asset on double-click.
                        }
                        else
                        {
                            document.revealModule = session.selectedModule; document.focusDetails = true;
                        }
                    }
                    if (moduleRow.enabled) document.stackActions.push_back([this,&document,emitterId,moduleId,enabled = !module.enabled]
                    { if (!document.session.SetModuleEnabled(emitterId,moduleId,enabled)) m_state->error = document.session.ActionError(); });
                    if (moduleRow.removed) document.stackActions.push_back([&document,emitterId,moduleId]
                    { auto value = document.session.document.Asset(); for (auto& item : value.emitters) if (item.settings.id == emitterId) for (auto& stackValue : item.stacks) std::erase_if(stackValue.modules,[&](const auto& entry) { return entry.id == moduleId; }); document.session.document.Apply(std::move(value),"Delete module"); });
                    if (localModule && ImGui::BeginDragDropSource())
                    { ImGui::SetDragDropPayload("NLS_VFX_MODULE",moduleId.c_str(),moduleId.size()+1); ImGui::TextUnformatted(name.c_str()); ImGui::EndDragDropSource(); }
                    if (localStage && ImGui::BeginDragDropTarget())
                    {
                        if (const auto* payload = ImGui::AcceptDragDropPayload("NLS_VFX_MODULE"))
                            document.stackActions.push_back([this,&document,from = std::string(static_cast<const char*>(payload->Data)),to = stack.id,at = moduleIndex]
                            { auto value = document.session.document.Asset(); std::string error; if (author::MoveModule(value,from,to,at,error,&document.session.Library())) document.session.document.Apply(std::move(value),"Move module"); else m_state->error = error; });
                        ImGui::EndDragDropTarget();
                    }
                    if (ImGui::BeginPopupContextItem("ModuleState"))
                    {
                        if (ImGui::MenuItem("Copy","Ctrl+C")) ExecuteCommand(&document,"Edit.Copy");
                        if (ImGui::MenuItem("Paste","Ctrl+V",false,!VFXTools::VfxClipboard::Get().Empty())) ExecuteCommand(&document,"Edit.Paste");
                        ImGui::Separator();
                        const auto selectedModules = session.SelectedModules();
                        if (selectedModules.size() > 1 && session.IsModuleSelected(moduleId))
                        {
                            const auto count = std::to_string(selectedModules.size());
                            if (ImGui::MenuItem(("Enable "+count+" selected").c_str())) document.stackActions.push_back([&document,selectedModules]
                            { document.properties.EndEdit(); document.session.document.EndEdit(); document.session.SetModulesEnabled(selectedModules,true); });
                            if (ImGui::MenuItem(("Disable "+count+" selected").c_str())) document.stackActions.push_back([&document,selectedModules]
                            { document.properties.EndEdit(); document.session.document.EndEdit(); document.session.SetModulesEnabled(selectedModules,false); });
                            if (ImGui::MenuItem(("Duplicate "+count+" selected").c_str())) document.stackActions.push_back([&document,selectedModules]
                            { const auto ids = document.session.DuplicateModules(selectedModules); if (!ids.empty()) document.session.SelectModules(ids); });
                            if (ImGui::MenuItem(("Delete "+count+" selected").c_str())) document.stackActions.push_back([&document,selectedModules]
                            { document.properties.EndEdit(); document.session.document.EndEdit(); document.session.DeleteModules(selectedModules); });
                            ImGui::Separator();
                        }
                        if (ImGui::MenuItem(module.enabled ? "Disable" : "Enable")) document.stackActions.push_back([this,&document,emitterId,moduleId,enabled = !module.enabled]
                        { if (!document.session.SetModuleEnabled(emitterId,moduleId,enabled)) m_state->error = document.session.ActionError(); });
                        if (ImGui::MenuItem("Edit module inputs"))
                        { document.properties.EndEdit(); session.SelectModule(emitterId,stackId,moduleId); document.revealModule = session.selectedModule; document.focusDetails = true; }
                        if (ImGui::MenuItem("Duplicate",nullptr,false,localModule)) document.stackActions.push_back([this,&document,moduleId]
                        { auto value = document.session.document.Asset(); std::string error; const auto id = author::DuplicateModule(value,moduleId,error); if (id.empty()) m_state->error = error; else if (document.session.document.Apply(std::move(value),"Duplicate module")) document.session.SelectModule(document.session.selectedEmitter,document.session.selectedStack,id); });
                        const auto move = [&](const int direction)
                        {
                            document.stackActions.push_back([this,&document,moduleId,direction]
                            {
                                auto value = document.session.document.Asset();
                                for (auto& item : value.emitters) for (auto& stackValue : item.stacks)
                                    for (size_t index = 0; index < stackValue.modules.size(); ++index) if (stackValue.modules[index].id == moduleId)
                                    {
                                        const auto destination = direction < 0 ? (index > 0 ? index-1 : 0) : index+2;
                                        std::string error;
                                        if (author::MoveModule(value,moduleId,stackValue.id,destination,error,&document.session.Library())) document.session.document.Apply(std::move(value),"Move module");
                                        else m_state->error = error;
                                        return;
                                    }
                            });
                        };
                        const auto localIndex = localModule ? moduleIndex : 0;
                        if (ImGui::MenuItem("Move Up",nullptr,false,localModule && localIndex > 0)) move(-1);
                        if (ImGui::MenuItem("Move Down",nullptr,false,localModule && localIndex+1 < stack.modules.size())) move(1);
                        if (ImGui::MenuItem("Delete",nullptr,false,localModule)) document.stackActions.push_back([&document,moduleId]
                        { auto value = document.session.document.Asset(); for (auto& item : value.emitters) for (auto& stackValue : item.stacks) std::erase_if(stackValue.modules,[&](const auto& entry) { return entry.id == moduleId; }); document.session.document.Apply(std::move(value),"Delete module"); });
                        if (!localModule && ImGui::MenuItem("Reset to Parent")) document.stackActions.push_back([&document,emitterId,moduleId]
                        { auto value = document.session.document.Asset(); for (auto& item : value.emitters) if (item.settings.id == emitterId) std::erase_if(item.overrides,[&](const auto& entry) { return entry.module == moduleId; }); document.session.document.Apply(std::move(value),"Reset inherited module"); });
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::Separator(); ImGui::TextDisabled("  Renderers");
            const auto* renderer = emitter.settings.renderer == NLS::VFX::RendererType::Sprite ? "Sprite Renderer" : emitter.settings.renderer == NLS::VFX::RendererType::Mesh ? "Mesh Renderer" : "Ribbon Renderer";
            ImGui::TextUnformatted(renderer);
        }
        ed::EndNode(); ed::PopStyleColor(2); ImGui::PopID();
    }
    // Editor-only annotations: comments and groups render over the canvas and
    // are manipulated while the node editor is suspended.
    auto annotation = session.document.Asset();
    struct Annotation { size_t index; bool group; ImVec2 minimum, maximum; };
    std::vector<Annotation> annotations;
    for (size_t index = 0; index < annotation.comments.size(); ++index)
    {
        const auto& comment = annotation.comments[index];
        annotations.push_back({index,false,
            ed::CanvasToScreen({comment.position[0],comment.position[1]}),
            ed::CanvasToScreen({comment.position[0]+comment.size[0],comment.position[1]+comment.size[1]})});
    }
    for (size_t index = 0; index < annotation.groups.size(); ++index)
    {
        bool any = false; ImVec2 minimum{1e9f,1e9f}, maximum{-1e9f,-1e9f};
        for (const auto& id : annotation.groups[index].emitters)
            for (const auto& emitter : asset.emitters) if (emitter.settings.id == id)
            {
                const auto canvasPosition = ed::GetNodePosition(ed::NodeId(NodeKey(id)));
                const auto canvasSize = ed::GetNodeSize(ed::NodeId(NodeKey(id)));
                const auto minimumPoint = ed::CanvasToScreen(canvasPosition);
                const auto maximumPoint = ed::CanvasToScreen({canvasPosition.x+canvasSize.x,canvasPosition.y+canvasSize.y});
                minimum = {(std::min)(minimum.x,minimumPoint.x-8),(std::min)(minimum.y,minimumPoint.y-26)};
                maximum = {(std::max)(maximum.x,maximumPoint.x+8),(std::max)(maximum.y,maximumPoint.y+8)};
                any = true;
            }
        if (any) annotations.push_back({index,true,minimum,maximum});
    }
    ed::Suspend();
    for (const auto& item : annotations)
    {
        const auto& group = item.group ? annotation.groups[item.index] : author::Group{};
        const auto& comment = item.group ? author::Comment{} : annotation.comments[item.index];
        const auto width = (std::max)(80.0f,item.maximum.x-item.minimum.x);
        const auto height = (std::max)(40.0f,item.maximum.y-item.minimum.y);
        ImGui::PushID(item.group ? ("group:"+group.id).c_str() : ("comment:"+comment.id).c_str());
        auto* draw = ImGui::GetWindowDrawList();
        if (item.group)
        {
            draw->AddRectFilled(item.minimum,{item.minimum.x+width,item.minimum.y+height},IM_COL32(40,48,64,40),4.0f);
            draw->AddRect(item.minimum,{item.minimum.x+width,item.minimum.y+height},IM_COL32(120,150,190,180),4.0f,0,1.5f);
            draw->AddRectFilled(item.minimum,{item.minimum.x+width,item.minimum.y+20},IM_COL32(60,80,110,200),4.0f,ImDrawFlags_RoundCornersTop);
            draw->AddText({item.minimum.x+6,item.minimum.y+3},IM_COL32(230,235,245,255),group.title.c_str());
        }
        else
        {
            const auto color = ImGui::ColorConvertU32ToFloat4(comment.color);
            draw->AddRectFilled(item.minimum,{item.minimum.x+width,item.minimum.y+height},
                ImGui::ColorConvertFloat4ToU32({color.x,color.y,color.z,0.45f}),3.0f);
            draw->AddRect(item.minimum,{item.minimum.x+width,item.minimum.y+height},
                ImGui::ColorConvertFloat4ToU32({color.x,color.y,color.z,0.9f}),3.0f,0,1.5f);
            draw->AddRectFilled(item.minimum,{item.minimum.x+width,item.minimum.y+20},
                ImGui::ColorConvertFloat4ToU32({color.x,color.y,color.z,1.0f}),3.0f,ImDrawFlags_RoundCornersTop);
            if (document.commentEditing == comment.id)
            {
                if (document.commentFocus) { ImGui::SetKeyboardFocusHere(); document.commentFocus = false; }
                ImGui::SetCursorScreenPos({item.minimum.x+4,item.minimum.y+1});
                ImGui::SetNextItemWidth(width-8);
                const bool commit = ImGui::InputText("##CommentText",document.commentText.data(),document.commentText.size(),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                if (commit || ImGui::IsItemDeactivatedAfterEdit())
                {
                    auto value = session.document.Asset();
                    for (auto& entry : value.comments) if (entry.id == comment.id) entry.text = document.commentText.data();
                    session.document.Apply(std::move(value),"Rename comment");
                    document.commentEditing.clear();
                }
            }
            else draw->AddText({item.minimum.x+6,item.minimum.y+3},IM_COL32(235,238,245,255),comment.text.c_str());
        }
        const bool editing = !item.group && document.commentEditing == comment.id;
        if (!editing)
        {
        ImGui::SetCursorScreenPos(item.minimum);
        ImGui::InvisibleButton("##AnnotationHeader",{width,20});
        if (!item.group)
        {
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                document.commentEditing = comment.id;
                std::snprintf(document.commentText.data(),document.commentText.size(),"%s",comment.text.c_str());
                document.commentFocus = true;
            }
            if (ImGui::IsItemActivated()) session.document.BeginEdit("Move comment");
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const auto zoom = (std::max)(ed::GetCurrentZoom(),0.0001f);
                annotation.comments[item.index].position[0] += ImGui::GetIO().MouseDelta.x/zoom;
                annotation.comments[item.index].position[1] += ImGui::GetIO().MouseDelta.y/zoom;
                session.document.PreviewEdit(annotation);
            }
            if (ImGui::IsItemDeactivated()) session.document.EndEdit();
            if (ImGui::BeginPopupContextItem("CommentActions"))
            {
                if (ImGui::MenuItem("Rename"))
                {
                    document.commentEditing = comment.id;
                    std::snprintf(document.commentText.data(),document.commentText.size(),"%s",comment.text.c_str());
                    document.commentFocus = true;
                }
                if (ImGui::MenuItem("Delete")) document.stackActions.push_back([&document,id = comment.id]
                {
                    auto value = document.session.document.Asset();
                    std::erase_if(value.comments,[&](const auto& entry) { return entry.id == id; });
                    document.session.document.Apply(std::move(value),"Delete comment");
                });
                ImGui::EndPopup();
            }
            // Bottom-right resize handle.
            ImGui::SetCursorScreenPos({item.minimum.x+width-12,item.minimum.y+height-12});
            ImGui::InvisibleButton("##AnnotationResize",{12,12});
            if (ImGui::IsItemActivated()) session.document.BeginEdit("Resize comment");
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const auto canvas = ed::ScreenToCanvas({item.minimum.x+width+ImGui::GetIO().MouseDelta.x,item.minimum.y+height+ImGui::GetIO().MouseDelta.y});
                annotation.comments[item.index].size = {
                    (std::max)(80.0f,canvas.x-annotation.comments[item.index].position[0]),
                    (std::max)(40.0f,canvas.y-annotation.comments[item.index].position[1])};
                session.document.PreviewEdit(annotation);
            }
            if (ImGui::IsItemDeactivated()) session.document.EndEdit();
        }
        else if (ImGui::BeginPopupContextItem("GroupActions"))
        {
            if (ImGui::MenuItem("Delete Group")) document.stackActions.push_back([&document,id = group.id]
            {
                auto value = document.session.document.Asset();
                std::erase_if(value.groups,[&](const auto& entry) { return entry.id == id; });
                document.session.document.Apply(std::move(value),"Delete group");
            });
            ImGui::EndPopup();
        }
        }
        ImGui::PopID();
    }
    ed::Resume();
    ed::Suspend();
    if (ed::HasSelectionChanged() && !rowSelected)
    {
        ed::NodeId selected{};
        if (ed::GetSelectedNodes(&selected,1) == 1)
        {
            if (selected.Get() == NodeKey(asset.id) && session.selection != VFXTools::VfxSelection::System) { document.properties.EndEdit(); session.SelectSystem(); }
            for (const auto& emitter : asset.emitters)
                if (selected.Get() == NodeKey(emitter.settings.id) && session.selectedEmitter != emitter.settings.id)
                { document.properties.EndEdit(); session.SelectEmitter(emitter.settings.id); }
        }
    }
    const auto selectedNode = session.selection == VFXTools::VfxSelection::System ? asset.id : session.selectedEmitter;
    if (document.overviewEmitter != selectedNode)
    { document.overviewEmitter = selectedNode; ed::SelectNode(ed::NodeId(NodeKey(selectedNode))); }
    ed::Resume(); ed::End();
    ImGui::PopStyleVar(); ed::PopStyleVar(5);
    auto candidate = session.document.Asset(); bool moved = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) for (auto& emitter : candidate.emitters)
    {
        const auto position = ed::GetNodePosition(ed::NodeId(NodeKey(emitter.settings.id)));
        const std::array logical{position.x/layoutScale,position.y/layoutScale};
        if (std::abs(logical[0]-emitter.position[0])+std::abs(logical[1]-emitter.position[1]) > 0.25f)
        { emitter.position = logical; moved = true; }
    }
    if (moved) session.document.Apply(std::move(candidate),"Move emitter");
    ed::SetCurrentEditor(nullptr);
    if (document.overviewModules.Draw(session)) { document.revealModule = session.selectedModule; document.focusDetails = true; }
}
void VfxEditor::Details(Document& document)
{
    auto& session = document.session; auto candidate = session.document.Asset();
    VFXTools::EnsureVfxInspectorReflection();
    document.properties.SetLifetime(session.document.Lifetime());
    const auto commitProperties = [&](const Inspector::ReflectedPropertyEdit& edit,const char* label)
    {
        if (!VFXTools::CommitInspectorEdit(session.document,candidate,edit,label))
            m_state->error = session.document.Error().empty() ? "Particle properties contain an invalid value." : session.document.Error();
    };
    if (session.selection == VFXTools::VfxSelection::System)
    {
        ImGui::SeparatorText(candidate.name.c_str());
        auto& timing = candidate.timing; const auto warmup = timing.WarmupTime();
        VFXTools::VfxSystemDetails fields{timing.fixedTick,timing.fixedTickDelta,warmup,timing.warmupTickCount,timing.warmupTickDelta};
        Inspector::ReflectedPropertyOptions options;
        if (!timing.fixedTick) options.disabled = {"fixedTickDelta"};
        if (!timing.warmupTickCount && timing.warmupTickDelta != 0) options.hidden = {"warmupTickCount","warmupTickDelta"};
        auto edit = document.properties.Draw("System","System",fields,options);
        if (edit.changed)
        {
            timing = {fields.fixedTick,fields.fixedTickDelta,fields.warmupTickCount,fields.warmupTickDelta};
            if ((edit.Changed("warmupTime") || edit.Changed("warmupTickDelta")) &&
                !NLS::VFX::SetSystemWarmupTime(timing,edit.Changed("warmupTime") ? fields.warmupTime : warmup))
            { edit.changed = false; m_state->error = "Warmup time exceeds the supported tick budget"; }
        }
        commitProperties(edit,"Edit System properties");
        return;
    }
    auto* emitter = Selected(candidate,session);
    if (!emitter) { ImGui::TextDisabled("Select an emitter or module in System Overview."); return; }
    const auto emitterId = emitter->settings.id;
    const auto editing = session.EditingSystem();
    const auto view = std::find_if(editing.emitters.begin(),editing.emitters.end(),[&](const auto& item) { return item.settings.id == emitterId; });
    if (view == editing.emitters.end()) return;
    const bool reveal = !document.revealModule.empty();
    if (reveal && document.revealModule == session.selectedModule) document.moduleSearch.clear();
    ImGui::PushID(emitterId.c_str());
    ImGui::TextUnformatted(view->settings.name.c_str());
    ImGui::TextDisabled("%s simulation",view->settings.target == NLS::VFX::SimulationTarget::CPU ? "CPU" : "GPU");
    if (session.SelectedModules().size() > 1) ImGui::TextDisabled("%zu modules selected",session.SelectedModules().size());
    char search[512]{}; std::memcpy(search,document.moduleSearch.data(),(std::min)(document.moduleSearch.size(),sizeof(search)-1));
    ImGui::SetNextItemWidth(-1);
    if (std::exchange(document.focusSearch,false)) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputTextWithHint("##SearchStack","Search stack...",search,sizeof(search))) document.moduleSearch = search;
    size_t visibleModules = 0, filteredModules = 0;
    const bool all = session.selection == VFXTools::VfxSelection::Emitter;
    if (!all && ImGui::SmallButton("Show emitter stack")) { document.properties.EndEdit(); session.SelectEmitter(emitterId); }
    if (all && ImGui::CollapsingHeader("Emitter Properties"))
    {
        VFXTools::VfxEmitterDetails fields{emitter->settings.name,emitter->enabled,view->settings.target,
            view->settings.capacity,view->settings.sortByViewDepth,view->settings.frustumCulling};
        const auto edit = document.properties.Draw("Emitter:"+emitterId,"Emitter",fields);
        if (edit.changed)
        {
            emitter->settings.name = fields.name; emitter->enabled = fields.enabled;
            emitter->settings.target = fields.target; emitter->settings.capacity = fields.capacity;
            emitter->settings.sortByViewDepth = fields.sortByViewDepth; emitter->settings.frustumCulling = fields.frustumCulling;
            if (!emitter->parent.Empty()) for (const auto* field : {"target","capacity","sortByViewDepth","frustumCulling"})
                if (edit.Changed(field) && std::find(emitter->settingsOverrides.begin(),emitter->settingsOverrides.end(),field) == emitter->settingsOverrides.end()) emitter->settingsOverrides.push_back(field);
        }
        commitProperties(edit,"Edit emitter properties");
        ImGui::Checkbox("Isolate",&document.isolate);
        if (ImGui::Button("Duplicate Emitter"))
        {
            auto copy = *emitter; copy.settings.id = Guid::New().ToString(); copy.settings.name += " Copy"; copy.position[0] += 330;
            copy.settings.spawn.id = Guid::New().ToString(); copy.settings.update.id = Guid::New().ToString();
            for (auto& stack : copy.stacks)
            {
                stack.id = Guid::New().ToString(); stack.inheritedId.clear();
                for (auto& module : stack.modules)
                { module.id = Guid::New().ToString(); module.inheritedId.clear(); for (auto& [name,input] : module.inputs) if (input.mode == author::InputMode::DynamicInput) input.expression.id = Guid::New().ToString(); }
            }
            const auto id = copy.settings.id; candidate.emitters.push_back(std::move(copy));
            document.properties.EndEdit(); session.document.EndEdit();
            if (session.document.Apply(std::move(candidate),"Duplicate emitter")) session.SelectEmitter(id);
            ImGui::PopID(); return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Emitter"))
        {
            std::erase_if(candidate.emitters,[&](const auto& item) { return item.settings.id == emitterId; });
            document.properties.EndEdit(); session.document.EndEdit();
            session.document.Apply(std::move(candidate),"Delete emitter"); session.ReconcileSelection(); ImGui::PopID(); return;
        }
    }
    if (all && !emitter->parent.Empty())
    {
        if (const auto* source = session.Library().EmitterAsset(emitter->parent.asset))
        {
            Inspector::ReflectedPropertyOptions versions;
            for (const auto& version : source->versions) versions.choices.push_back({version.id,std::to_string(version.major)+"."+std::to_string(version.minor)});
            const auto edit = document.properties.Draw("EmitterVersion:"+emitterId,"Source Version",emitter->parent.version,versions);
            commitProperties(edit,"Change emitter version");
        }
    }
    const size_t stageCount = std::count_if(view->stacks.begin(),view->stacks.end(),[](const auto& stack) { return stack.stage == author::Stage::Simulation; });
    const size_t localStageCount = std::count_if(emitter->stacks.begin(),emitter->stacks.end(),[](const auto& stack) { return stack.stage == author::Stage::Simulation; });
    if (all && view->settings.target == NLS::VFX::SimulationTarget::GPU && stageCount < 64 && ImGui::SmallButton("+ Simulation Stage"))
        document.stackActions.push_back([&document]
        { auto system = document.session.document.Asset(); if (auto* item = Selected(system,document.session)) { item->stacks.push_back(author::CreateSimulationStage()); document.session.document.Apply(std::move(system),"Add Simulation Stage"); } });

    for (const auto& stack : view->stacks)
    {
        const auto stackId = std::string(VFXTools::VfxSourceId(stack));

        const auto localStack = std::find_if(emitter->stacks.begin(),emitter->stacks.end(),[&](const auto& item)
        { return item.id == stackId || (stack.stage != author::Stage::Simulation && stack.stage != author::Stage::Event && item.stage == stack.stage); });
        const auto stageIndex = static_cast<size_t>(std::count_if(emitter->stacks.begin(),localStack,[](const auto& item) { return item.stage == author::Stage::Simulation; }));
        ImGui::PushID(stackId.c_str());
        const auto stackKey = emitterId+":"+stackId;
        if (reveal && session.selectedStack == stackId) document.collapsedDetails[stackKey] = false;
        VFXTools::VfxStackRow stage; stage.label = stack.stage == author::Stage::Simulation ? stack.name.c_str() : author::StageName(stack.stage);
        stage.width = ImGui::GetContentRegionAvail().x; stage.header = true; stage.expandable = true; stage.add = true;
        stage.expanded = !document.collapsedDetails[stackKey]; stage.enabled = stack.enabled;
        stage.selected = session.selection == VFXTools::VfxSelection::Stack && session.selectedStack == stackId;
        stage.accent = VFXTools::VfxStyle::StageIconColor(stack.stage);
        const auto stageRow = VFXTools::DrawVfxStackRow(stage);
        if (stageRow.expanded) document.collapsedDetails[stackKey] = !document.collapsedDetails[stackKey];
        if (stageRow.add) { document.properties.EndEdit(); session.document.EndEdit(); document.detailsModules.Open(emitterId,stackId); }
        if (stageRow.selected) { document.properties.EndEdit(); session.SelectStack(emitterId,stackId); }
        if (stack.stage == author::Stage::Simulation && localStack != emitter->stacks.end() && ImGui::BeginPopupContextItem("StageActions"))
        {
            if (ImGui::MenuItem("Duplicate",nullptr,false,stageCount < 64)) document.stackActions.push_back([this,&document,id = localStack->id]
            { auto value = document.session.document.Asset(); std::string error; if (author::DuplicateSimulationStage(*Selected(value,document.session),id,error).empty()) m_state->error = error; else document.session.document.Apply(std::move(value),"Duplicate Simulation Stage"); });
            const auto move = [&](size_t index) { document.stackActions.push_back([this,&document,id = localStack->id,index]
            { auto value = document.session.document.Asset(); std::string error; if (author::MoveSimulationStage(*Selected(value,document.session),id,index,error)) document.session.document.Apply(std::move(value),"Move Simulation Stage"); else m_state->error = error; }); };
            if (ImGui::MenuItem("Move Up",nullptr,false,stageIndex > 0)) move(stageIndex-1);
            if (ImGui::MenuItem("Move Down",nullptr,false,stageIndex+1 < localStageCount)) move(stageIndex+1);
            if (ImGui::MenuItem("Delete")) document.stackActions.push_back([&document,id = localStack->id]
            { auto value = document.session.document.Asset(); if (auto* selected = Selected(value,document.session)) { std::erase_if(selected->stacks,[&](const auto& item) { return item.id == id; }); document.session.document.Apply(std::move(value),"Delete Simulation Stage"); } });
            ImGui::EndPopup();
        }
        if (stack.stage == author::Stage::Simulation && localStack != emitter->stacks.end())
        {
            if (ImGui::BeginDragDropSource())
            { ImGui::SetDragDropPayload("NLS_VFX_STAGE",localStack->id.c_str(),localStack->id.size()+1); ImGui::TextUnformatted(stack.name.c_str()); ImGui::EndDragDropSource(); }
            if (ImGui::BeginDragDropTarget())
            {
                if (const auto* payload = ImGui::AcceptDragDropPayload("NLS_VFX_STAGE"))
                {
                    const auto destination = static_cast<size_t>(std::count_if(emitter->stacks.begin(),localStack,[](const auto& item) { return item.stage == author::Stage::Simulation; }));
                    document.stackActions.push_back([this,&document,emitterId,from = std::string(static_cast<const char*>(payload->Data)),destination]
                    { auto value = document.session.document.Asset(); std::string error; for (auto& item : value.emitters) if (item.settings.id == emitterId) { if (author::MoveSimulationStage(item,from,destination,error)) document.session.document.Apply(std::move(value),"Move Simulation Stage"); else m_state->error = error; break; } });
                }
                ImGui::EndDragDropTarget();
            }
        }
        if (!document.collapsedDetails[stackKey])
        {
            if (stack.stage == author::Stage::Simulation)
            {
                VFXTools::VfxStageDetails fields{stack.enabled,stack.name,stack.iterations};
                Inspector::ReflectedPropertyOptions options; if (!stack.bindings.iterations.empty()) options.disabled = {"iterations"};
                const auto stageCommit = [&](const Inspector::ReflectedPropertyEdit& edit,const char* label)
                {
                    if (edit.changed)
                    {
                        if (localStack != emitter->stacks.end() && emitter->parent.Empty()) { localStack->enabled = fields.enabled; localStack->name = fields.name; localStack->iterations = fields.iterations; }
                        else
                        {
                            auto item = std::find_if(emitter->stackOverrides.begin(),emitter->stackOverrides.end(),[&](const auto& value) { return value.stack == stackId; });
                            if (item == emitter->stackOverrides.end()) { emitter->stackOverrides.push_back({stackId}); item = std::prev(emitter->stackOverrides.end()); }
                            if (edit.Changed("enabled")) item->enabled = fields.enabled;
                            if (edit.Changed("name")) item->name = fields.name;
                            if (edit.Changed("iterations")) item->iterations = fields.iterations;
                        }
                    }
                    commitProperties(edit,label);
                };
                const auto edit = document.properties.Draw("Stage:"+stackId,"Simulation Stage",fields,options);
                stageCommit(edit,"Edit Simulation Stage properties");
                auto bindings = stack.bindings;
                VFXTools::DrawStageControlBindings(document.properties,bindings,candidate.parameters,false,[&](const auto& edit,const char* label)
                {
                    if (edit.changed)
                    {
                        if (localStack != emitter->stacks.end() && emitter->parent.Empty()) localStack->bindings = bindings;
                        else { auto item = std::find_if(emitter->stackOverrides.begin(),emitter->stackOverrides.end(),[&](const auto& value) { return value.stack == stackId; }); if (item == emitter->stackOverrides.end()) { emitter->stackOverrides.push_back({stackId}); item = std::prev(emitter->stackOverrides.end()); } item->bindings = bindings; }
                    }
                    commitProperties(edit,label);
                });
                if (!emitter->parent.Empty() && std::any_of(emitter->stackOverrides.begin(),emitter->stackOverrides.end(),[&](const auto& item) { return item.stack == stackId; }))
                    if (ImGui::SmallButton("Reset Stage to Parent")) document.stackActions.push_back([&document,emitterId,stackId]
                    { auto value = document.session.document.Asset(); for (auto& item : value.emitters) if (item.settings.id == emitterId) std::erase_if(item.stackOverrides,[&](const auto& entry) { return entry.stack == stackId; }); document.session.document.Apply(std::move(value),"Reset inherited stage"); });
                ImGui::TextDisabled("Iteration Source: Particles");
            }
            for (size_t index = 0; index < stack.modules.size(); ++index)
            {
                auto module = stack.modules[index]; const auto moduleId = std::string(VFXTools::VfxSourceId(module)); module.id = moduleId;
                const auto* definition = author::FindModule(module.script);
                author::ModuleDefinition externalDefinition;
                const auto* scriptAsset = session.Library().Script(module.scriptAsset.asset);
                if (scriptAsset) if (const auto* version = author::SelectVersion(*scriptAsset,module.scriptAsset.version))
                {
                    externalDefinition.id = scriptAsset->id; externalDefinition.name = scriptAsset->name; externalDefinition.stage = stack.stage;
                    for (const auto& input : version->inputs) externalDefinition.inputs.push_back({input.name,input.type,input.defaultValue,input.metadata});
                    definition = &externalDefinition;
                }
                const auto label = definition ? definition->name : module.script;
                if (!VFXTools::MatchesVfxModule(module,label,definition,document.moduleSearch)) { ++filteredModules; continue; }
                ++visibleModules;
                std::string badge;
                if (scriptAsset) for (const auto& version : scriptAsset->versions)
                    if (version.id == module.scriptAsset.version && (version.major > 1 || version.minor > 0))
                    { badge = "v"+std::to_string(version.major)+"."+std::to_string(version.minor); break; }
                const auto summary = VFXTools::VfxInputSummary(module,definition);
                author::Module* localModule = nullptr;
                if (localStack != emitter->stacks.end()) for (auto& item : localStack->modules) if (item.id == moduleId) localModule = &item;
                ImGui::PushID(moduleId.c_str());
                if (document.revealModule == moduleId) document.collapsedDetails[emitterId+":"+moduleId] = false;
                VFXTools::VfxStackRow row; row.label = label.c_str(); row.width = ImGui::GetContentRegionAvail().x;
                row.summary = summary.c_str(); row.badge = badge.empty() ? nullptr : badge.c_str();
                row.expandable = true; row.expanded = !document.collapsedDetails[emitterId+":"+moduleId]; row.enable = true; row.enabled = module.enabled;
                row.selected = session.IsModuleSelected(moduleId); row.inherited = !localModule; row.accent = stage.accent;
                row.remove = localModule != nullptr; row.muted = !emitter->enabled || !stack.enabled;
                row.issue = std::any_of(session.Issues().begin(),session.Issues().end(),[&](const auto& issue) { return issue.emitter == emitterId && (issue.module == moduleId || issue.module == stack.modules[index].id); });
                const auto moduleRow = VFXTools::DrawVfxStackRow(row);
                if (document.revealModule == moduleId) { ImGui::SetScrollHereY(0.15f); document.revealModule.clear(); }
                if (moduleRow.expanded) document.collapsedDetails[emitterId+":"+moduleId] = !document.collapsedDetails[emitterId+":"+moduleId];
                if (moduleRow.selected) { document.properties.EndEdit(); session.SelectModule(emitterId,stackId,moduleId,ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift); }
                if (moduleRow.doubleClicked && OpenModuleSource(document,module)) { /* UE opens the module script asset on double-click. */ }
                if (moduleRow.enabled) document.stackActions.push_back([this,&document,emitterId,moduleId,enabled = !module.enabled]
                { if (!document.session.SetModuleEnabled(emitterId,moduleId,enabled)) m_state->error = document.session.ActionError(); });
                if (moduleRow.removed) document.stackActions.push_back([&document,emitterId,moduleId]
                { auto value = document.session.document.Asset(); for (auto& item : value.emitters) if (item.settings.id == emitterId) for (auto& stack : item.stacks) std::erase_if(stack.modules,[&](const auto& module) { return module.id == moduleId; }); document.session.document.Apply(std::move(value),"Delete module"); });
                if (localModule && ImGui::BeginDragDropSource())
                { ImGui::SetDragDropPayload("NLS_VFX_MODULE",moduleId.c_str(),moduleId.size()+1); ImGui::TextUnformatted(label.c_str()); ImGui::EndDragDropSource(); }
                if (localStack != emitter->stacks.end() && ImGui::BeginDragDropTarget())
                {
                    if (const auto* payload = ImGui::AcceptDragDropPayload("NLS_VFX_MODULE"))
                    {
                        const auto at = localModule ? static_cast<size_t>(localModule-localStack->modules.data()) : size_t{0};
                        document.stackActions.push_back([this,&document,from = std::string(static_cast<const char*>(payload->Data)),to = localStack->id,at]
                        { auto value = document.session.document.Asset(); std::string error; if (author::MoveModule(value,from,to,at,error,&document.session.Library())) document.session.document.Apply(std::move(value),"Move module"); else m_state->error = error; });
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::BeginPopupContextItem("ModuleActions"))
                {
                    if (ImGui::MenuItem("Copy","Ctrl+C")) ExecuteCommand(&document,"Edit.Copy");
                    if (ImGui::MenuItem("Paste","Ctrl+V",false,!VFXTools::VfxClipboard::Get().Empty())) ExecuteCommand(&document,"Edit.Paste");
                    ImGui::Separator();
                    const auto selectedModules = session.SelectedModules();
                    if (selectedModules.size() > 1 && session.IsModuleSelected(moduleId))
                    {
                        const auto count = std::to_string(selectedModules.size());
                        if (ImGui::MenuItem(("Enable "+count+" selected").c_str())) document.stackActions.push_back([&document,selectedModules]
                        { document.properties.EndEdit(); document.session.document.EndEdit(); document.session.SetModulesEnabled(selectedModules,true); });
                        if (ImGui::MenuItem(("Disable "+count+" selected").c_str())) document.stackActions.push_back([&document,selectedModules]
                        { document.properties.EndEdit(); document.session.document.EndEdit(); document.session.SetModulesEnabled(selectedModules,false); });
                        if (ImGui::MenuItem(("Duplicate "+count+" selected").c_str())) document.stackActions.push_back([&document,selectedModules]
                        { const auto ids = document.session.DuplicateModules(selectedModules); if (!ids.empty()) document.session.SelectModules(ids); else document.session.ActionError(); });
                        if (ImGui::MenuItem(("Delete "+count+" selected").c_str())) document.stackActions.push_back([&document,selectedModules]
                        { document.properties.EndEdit(); document.session.document.EndEdit(); document.session.DeleteModules(selectedModules); });
                        ImGui::Separator();
                    }
                    if (ImGui::MenuItem(module.enabled ? "Disable" : "Enable")) document.stackActions.push_back([this,&document,emitterId,moduleId,enabled = !module.enabled]
                    { if (!document.session.SetModuleEnabled(emitterId,moduleId,enabled)) m_state->error = document.session.ActionError(); });
                    if (ImGui::MenuItem("Duplicate",nullptr,false,localModule != nullptr)) document.stackActions.push_back([this,&document,moduleId]
                    { auto value = document.session.document.Asset(); std::string error; const auto id = author::DuplicateModule(value,moduleId,error); if (id.empty()) m_state->error = error; else if (document.session.document.Apply(std::move(value),"Duplicate module")) document.session.SelectModule(document.session.selectedEmitter,document.session.selectedStack,id); });
                    const auto localIndex = localModule ? static_cast<size_t>(localModule-localStack->modules.data()) : size_t{0};
                    const auto move = [&](size_t destination)
                    { document.stackActions.push_back([this,&document,emitterId,moduleId,to = localStack->id,destination]
                      { auto value = document.session.document.Asset(); std::string error; if (author::MoveModule(value,moduleId,to,destination,error,&document.session.Library())) document.session.document.Apply(std::move(value),"Move module"); else m_state->error = error; }); };
                    if (ImGui::MenuItem("Move Up",nullptr,false,localModule && localIndex > 0)) move(localIndex-1);
                    if (ImGui::MenuItem("Move Down",nullptr,false,localModule && localIndex+1 < localStack->modules.size())) move(localIndex+2);
                    if (ImGui::MenuItem("Delete",nullptr,false,localModule != nullptr)) document.stackActions.push_back([&document,moduleId]
                    { auto value = document.session.document.Asset(); for (auto& emitter : value.emitters) for (auto& stack : emitter.stacks) std::erase_if(stack.modules,[&](const auto& item) { return item.id == moduleId; }); document.session.document.Apply(std::move(value),"Delete module"); });
                    if (!localModule && ImGui::MenuItem("Reset to Parent")) document.stackActions.push_back([&document,emitterId,moduleId]
                    { auto value = document.session.document.Asset(); for (auto& emitter : value.emitters) if (emitter.settings.id == emitterId) std::erase_if(emitter.overrides,[&](const auto& item) { return item.module == moduleId; }); document.session.document.Apply(std::move(value),"Reset inherited module"); });
                    ImGui::EndPopup();
                }
                if (!document.collapsedDetails[emitterId+":"+moduleId])
                {
                    ImGui::BeginDisabled(!emitter->enabled || !stack.enabled || !module.enabled);
                    if (scriptAsset)
                    {
                        Inspector::ReflectedPropertyOptions versions;
                        for (const auto& version : scriptAsset->versions) versions.choices.push_back({version.id,std::to_string(version.major)+"."+std::to_string(version.minor)});
                        ImGui::BeginDisabled(!localModule);
                        const auto versionEdit = document.properties.Draw("ModuleVersion:"+moduleId,"Version",module.scriptAsset.version,versions);
                        if (versionEdit.changed && localModule) localModule->scriptAsset.version = module.scriptAsset.version;
                        commitProperties(versionEdit,"Change module script version");
                        ImGui::EndDisabled();
                        if (const auto dependency = session.Library().Dependency(scriptAsset->id); dependency && !dependency->path.empty())
                            if (ImGui::SmallButton("Open Script Asset")) EDITOR_PANEL(Editor::Panels::VfxLibraryEditor,"Particle Library Editor").OpenDocument(dependency->path);
                    }
                    if (definition) for (const auto& inputDefinition : definition->inputs)
                    {
                        const auto& metadata = inputDefinition.metadata;
                        // Visible-If metadata mirrors Niagara's edit conditions.
                        // Fail open when the controlling input is not literal yet.
                        if (!metadata.visibleIf.empty() && metadata.visibleIf != inputDefinition.name)
                        {
                            const auto control = module.inputs.find(metadata.visibleIf);
                            if (control != module.inputs.end() && control->second.mode == author::InputMode::Literal &&
                                control->second.value[0] == 0) continue;
                        }
                        auto& input = module.inputs.try_emplace(inputDefinition.name,author::Input{author::InputMode::Literal,inputDefinition.defaultValue}).first->second; ImGui::PushID(inputDefinition.name.c_str());
                        input.type = inputDefinition.type;
                        const auto commitModule = [&](const Inspector::ReflectedPropertyEdit& edit,const char* label)
                        {
                            if (edit.changed && !VFXTools::StoreVfxModuleInput(*emitter,module,inputDefinition.name,input))
                            { m_state->error = "The module input is no longer available."; return; }
                            commitProperties(edit,label);
                        };
                        // Per-input diff/override state. A local module marks a
                        // value that differs from its default; an inherited
                        // module can additionally carry an emitter override.
                        const bool inheritedOverride = !localModule && !emitter->parent.Empty() &&
                            std::any_of(emitter->overrides.begin(),emitter->overrides.end(),[&](const auto& value)
                            { return value.module == moduleId && value.inputs.contains(inputDefinition.name); });
                        const bool modified = input.mode != author::InputMode::Literal ||
                            input.value != inputDefinition.defaultValue || !input.parameter.empty() ||
                            !input.scriptAsset.Empty() || !input.curve.empty() || !input.expression.nodes.empty();
                        const bool canReset = modified || inheritedOverride;
                        const auto resetInput = [&]
                        {
                            author::Input reset{author::InputMode::Literal,inputDefinition.defaultValue};
                            reset.type = inputDefinition.type;
                            input = reset;
                            if (!VFXTools::ResetVfxModuleInput(*emitter,module,inputDefinition.name,input))
                            { m_state->error = "The module input is no longer available."; return; }
                            Commit(session,candidate,true,"Reset module input");
                        };
                        const auto changeMode = [&](author::InputMode mode)
                        {
                            if (mode == input.mode) return;
                            if (mode == author::InputMode::Script && input.scriptAsset.Empty())
                            {
                                for (const auto& [id,script] : session.Library().Scripts()) if (script.kind != author::AssetKind::Module)
                                    if (const auto* version = author::SelectVersion(script,{}))
                                        if (std::any_of(version->graph.nodes.begin(),version->graph.nodes.end(),[&](const auto& node) { return node.operation == NLS::VFX::Operation::WriteAttribute && node.type == inputDefinition.type; }))
                                        { input.scriptAsset = {id,version->id}; break; }
                                if (input.scriptAsset.Empty()) { m_state->error = "Create a compatible Function or Dynamic Input asset first."; return; }
                            }
                            if (mode == author::InputMode::Curve && NLS::VFX::IsIntegralType(inputDefinition.type)) { m_state->error = "Curves require a floating point input."; return; }
                            if (mode == author::InputMode::Curve && input.curve.empty()) input.curve = {{0,input.value},{1,input.value}};
                            if (mode == author::InputMode::DynamicInput && input.expression.nodes.empty())
                            {
                                input.expression.id = Guid::New().ToString();
                                input.expression.stage = stack.stage == author::Stage::Simulation ? NLS::VFX::ScriptStage::SimulationStage : stack.stage == author::Stage::ParticleSpawn ? NLS::VFX::ScriptStage::ParticleSpawn : NLS::VFX::ScriptStage::ParticleUpdate;
                                const auto value = NLS::VFX::AddNode(input.expression,NLS::VFX::Operation::Constant,inputDefinition.type);
                                NLS::VFX::FindNode(input.expression,value)->value = input.value;
                                const auto output = NLS::VFX::AddNode(input.expression,NLS::VFX::Operation::WriteAttribute,inputDefinition.type);
                                NLS::VFX::FindNode(input.expression,output)->name = "Output"; NLS::VFX::FindNode(input.expression,output)->inputs = {value};
                                NLS::VFX::FindNode(input.expression,output)->position = {300,0};
                            }
                            input.mode = mode;
                            Inspector::ReflectedPropertyEdit edit; edit.changed = true;
                            commitModule(edit,"Change input source");
                        };
                        const float rowScale = ImGui::GetFontSize()/13.0f;
                        const float modeWidth = 76.0f*rowScale;
                        if (!ImGui::BeginTable("##ModuleInput", 3, ImGuiTableFlags_SizingStretchProp))
                        {
                            ImGui::PopID();
                            continue;
                        }
                        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, modeWidth);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        if (canReset)
                        {
                            if (ImGui::SmallButton("Reset")) resetInput();
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(inheritedOverride ? "Clear inherited override" : "Reset to default");
                            ImGui::SameLine();
                        }
                        std::string inputLabel = metadata.displayName.empty() ? inputDefinition.name : metadata.displayName;
                        if (!metadata.unit.empty()) inputLabel += " (" + metadata.unit + ")";
                        ImGui::TextUnformatted(inputLabel.c_str());
                        if (ImGui::IsItemHovered() && (!metadata.tooltip.empty() || metadata.hasRange || metadata.step > 0))
                        {
                            ImGui::BeginTooltip();
                            if (!metadata.tooltip.empty()) ImGui::TextUnformatted(metadata.tooltip.c_str());
                            if (metadata.hasRange) ImGui::TextDisabled("Range: %g to %g",metadata.minimum,metadata.maximum);
                            if (metadata.step > 0) ImGui::TextDisabled("Step: %g",metadata.step);
                            ImGui::EndTooltip();
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        bool edited = false;
                        if (input.mode == author::InputMode::Literal)
                        {
                            Inspector::ReflectedPropertyOptions options;
                            options.layout = Inspector::PropertyEditorLayout::ValueOnly;
                            options.metadata.emplace();
                            options.metadata->tooltip = metadata.tooltip;
                            options.metadata->colorUsage = inputDefinition.type == NLS::VFX::ValueType::Float4 && (inputDefinition.name.find("Color") != std::string::npos || inputDefinition.name.find("Tint") != std::string::npos);
                            if (metadata.hasRange)
                            {
                                options.metadata->rangeMin = metadata.minimum;
                                options.metadata->rangeMax = metadata.maximum;
                            }
                            if (metadata.step > 0)
                                options.metadata->step = metadata.step;
                            const auto edit = VFXTools::DrawVfxValue(document.properties, "##Value", inputDefinition.type, input.value, options);
                            commitModule(edit, "Edit module input");
                        }
                        else if (input.mode == author::InputMode::Parameter)
                        {
                            if (ImGui::BeginCombo("##Parameter",input.parameter.empty() ? "Select parameter" : input.parameter.c_str()))
                            {
                                if (ImGui::Selectable("(none)",input.parameter.empty())) { input.parameter.clear(); edited = true; }
                                for (const auto& parameter : candidate.parameters) if (parameter.type == inputDefinition.type)
                                    if (ImGui::Selectable(parameter.name.c_str(),input.parameter == parameter.name)) { input.parameter = parameter.name; edited = true; }
                                ImGui::EndCombo();
                            }
                        }
                        else if (input.mode == author::InputMode::Script)
                        {
                            const auto* selected = session.Library().Script(input.scriptAsset.asset);
                            if (ImGui::BeginCombo("##Script",selected ? selected->name.c_str() : "Select asset"))
                            {
                                for (const auto& [id,source] : session.Library().Scripts()) if (source.kind != author::AssetKind::Module && source.visible)
                                    if (ImGui::Selectable(source.name.c_str(),input.scriptAsset.asset == id))
                                    { input.scriptAsset = {id,source.exposedVersion}; input.scriptArguments.clear(); input.scriptArgumentTypes.clear(); edited = true; }
                                ImGui::EndCombo();
                            }
                        }
                        else if (input.mode == author::InputMode::Curve)
                        {
                            if (ImGui::Button("Edit Curve")) { session.SelectModule(emitter->settings.id,stack.id,module.id); document.focusCurves = true; }
                        }
                        else if (input.mode == author::InputMode::DynamicInput)
                        {
                            if (ImGui::Button("Edit Script")) { session.SelectModule(emitter->settings.id,stack.id,module.id); document.search = inputDefinition.name; document.showScript = true; document.focusScript = true; }
                        }
                        if (edited)
                        {
                            Inspector::ReflectedPropertyEdit edit;
                            edit.changed = true;
                            edit.completed = true;
                            commitModule(edit, "Edit module input");
                        }
                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::BeginCombo("##Mode",InputModeName(input.mode)))
                        {
                            for (const auto mode : {author::InputMode::Literal,author::InputMode::Parameter,author::InputMode::Curve,author::InputMode::DynamicInput,author::InputMode::Script})
                                if (ImGui::Selectable(InputModeName(mode),mode == input.mode)) changeMode(mode);
                            ImGui::EndCombo();
                        }
                        ImGui::EndTable();
                        if (input.mode == author::InputMode::Script)
                        {
                            const auto* script = session.Library().Script(input.scriptAsset.asset);
                            if (script)
                            {
                                Inspector::ReflectedPropertyOptions versions;
                                for (const auto& version : script->versions) versions.choices.push_back({version.id,std::to_string(version.major)+"."+std::to_string(version.minor)});
                                const auto versionEdit = document.properties.Draw("InputVersion:"+module.id+":"+inputDefinition.name,"Script Version",input.scriptAsset.version,versions);
                                commitModule(versionEdit,"Set dynamic script version");
                                if (const auto* version = author::SelectVersion(*script,input.scriptAsset.version)) for (const auto& field : version->inputs)
                                {
                                    auto value = input.scriptArguments.contains(field.name) ? input.scriptArguments.at(field.name) : field.defaultValue;
                                    const auto edit = VFXTools::DrawVfxValue(document.properties,field.name.c_str(),field.type,value);
                                    if (edit.changed) { input.scriptArguments[field.name] = value; input.scriptArgumentTypes[field.name] = field.type; }
                                    commitModule(edit,"Edit dynamic script input");
                                }
                            }
                        }
                        ImGui::PopID();
                    }
                    if (definition && !definition->requiresModule.empty())
                    {
                        const bool missing = std::none_of(stack.modules.begin(),stack.modules.begin()+index,[&](const auto& item) { return item.enabled && item.script == definition->requiresModule; });
                        if (missing)
                        {
                            ImGui::TextColored({1,0.6f,0.2f,1},"Requires %s",definition->requiresModule.c_str());
                            if (localModule && ImGui::SmallButton("Fix Issue")) document.stackActions.push_back([&document,id = localStack->id,required = definition->requiresModule,moduleId]
                            { auto value = document.session.document.Asset(); if (auto* emitter = Selected(value,document.session)) for (auto& stack : emitter->stacks) if (stack.id == id) { const auto at = std::find_if(stack.modules.begin(),stack.modules.end(),[&](const auto& item) { return item.id == moduleId; }); stack.modules.insert(at,author::CreateModule(required)); break; } document.session.document.Apply(std::move(value),"Fix module dependency"); });
                        }
                    }
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }
    if (!document.moduleSearch.empty()) ImGui::TextDisabled("%zu of %zu modules match",visibleModules,visibleModules+filteredModules);
    if (all && ImGui::CollapsingHeader("Renderers"))
    {
        ImGui::TextUnformatted(view->settings.renderer == NLS::VFX::RendererType::Sprite ? "Sprite Renderer" : view->settings.renderer == NLS::VFX::RendererType::Mesh ? "Mesh Renderer" : "Ribbon Renderer");
        const auto& color = emitter->settings.sprite.tint;
        VFXTools::VfxRendererDetails fields{Maths::Color(color[0],color[1],color[2],color[3]),emitter->settings.sprite.blend};
        const auto edit = document.properties.Draw("Renderer:"+emitterId,"Renderer",fields);
        emitter->settings.sprite.tint = {fields.color.r,fields.color.g,fields.color.b,fields.color.a}; emitter->settings.sprite.blend = fields.blend;
        commitProperties(edit,"Edit renderer properties");
    }
    ImGui::PopID();
    if (document.detailsModules.Draw(session)) { document.revealModule = session.selectedModule; document.focusDetails = true; }
}
void VfxEditor::Parameters(Document& document, bool overrides)
{
    auto& session = document.session; auto candidate = session.document.Asset();
    VFXTools::EnsureVfxInspectorReflection(); document.properties.SetLifetime(session.document.Lifetime());
    if (std::exchange(document.focusSearch,false)) ImGui::SetKeyboardFocusHere();
    Text("Search",document.parameterSearch);
    std::optional<size_t> removed;
    for (size_t index = 0; index < candidate.parameters.size(); ++index)
    {
        auto& parameter = candidate.parameters[index];
        if (!document.parameterSearch.empty() && parameter.name.find(document.parameterSearch) == std::string::npos) continue;
        ImGui::PushID(parameter.id.c_str());
        bool enabledControl = false, iterationControl = false;
        const auto controls = [&](const auto& emitters)
        { for (const auto& emitter : emitters) for (const auto& stack : emitter.stacks) { enabledControl |= stack.bindings.enabled == parameter.name; iterationControl |= stack.bindings.iterations == parameter.name; } };
        controls(candidate.emitters); controls(session.Resolved().emitters);
        const auto editValue = [&](const char* label,NLS::VFX::Value& value)
        {
            if (parameter.type == NLS::VFX::ValueType::Float && enabledControl)
            { bool enabled = value[0] != 0; const auto edit = document.properties.Draw(std::to_string(ImGui::GetID(label)),label,enabled); if (edit.changed) value[0] = enabled ? 1.0f : 0.0f; return edit; }
            if (parameter.type == NLS::VFX::ValueType::Float && iterationControl)
            {
                int count = static_cast<int>(std::clamp(value[0],0.0f,128.0f));
                const auto edit = document.properties.Draw(std::to_string(ImGui::GetID(label)),label,count);
                if (edit.changed) value[0] = static_cast<float>(std::clamp(count,0,128)); return edit;
            }
            return VFXTools::DrawVfxValue(document.properties,label,parameter.type,value);
        };
        if (overrides)
        {
            auto value = session.Parameters().contains(parameter.name) ? session.Parameters().at(parameter.name) : parameter.value;
            if (editValue(parameter.name.c_str(),value).changed && !session.SetParameter(parameter.name,value)) m_state->error = "The parameter value exceeds the Simulation Stage limits.";
        }
        else
        {
            const auto renamed = document.properties.Draw("Name:"+parameter.id,"Name",parameter.name);
            VFXTools::CommitInspectorEdit(session.document,candidate,renamed,"Rename parameter");
            const auto previousType = parameter.type;
            const auto typed = document.properties.Draw("Type:"+parameter.id,"Type",parameter.type);
            if (typed.changed && (NLS::VFX::IsIntegralType(previousType) || NLS::VFX::IsIntegralType(parameter.type))) parameter.value = {};
            VFXTools::CommitInspectorEdit(session.document,candidate,typed,"Change parameter type");
            const auto edit = editValue("Default",parameter.value);
            VFXTools::CommitInspectorEdit(session.document,candidate,edit,"Set parameter default");
            if (ImGui::Button("Delete")) removed = index;
        }
        ImGui::Separator(); ImGui::PopID();
    }
    if (!overrides && ImGui::Button("+ Parameter"))
    {
        size_t number = candidate.parameters.size()+1; std::string name;
        do { name = "User.Parameter"+std::to_string(number++); }
        while (std::any_of(candidate.parameters.begin(),candidate.parameters.end(),[&](const auto& parameter) { return parameter.name == name; }));
        candidate.parameters.push_back({Guid::New().ToString(),std::move(name),NLS::VFX::ValueType::Float,{}});
        session.document.Apply(std::move(candidate),"Add user parameter");
    }
    if (removed) { candidate.parameters.erase(candidate.parameters.begin()+*removed); session.document.Apply(std::move(candidate),"Delete user parameter"); }
}

void VfxEditor::Preview(Document& document)
{
    auto& session = document.session; auto& camera = document.camera;
    const bool hasDriver = NLS::Core::ServiceLocator::Contains<Render::Context::Driver>();
    if (!hasDriver || document.preview.CanAdvance()) session.Tick(ImGui::GetIO().DeltaTime);
    if (document.loopRange && session.playing && session.Program() && session.Instance().Age() >= session.rangeEnd)
        session.Reset();
    if (!hasDriver) (void)session.TakeFrames();
    // Viewport toolbar: display options plus the 3D camera (Perspective/Ortho,
    // FOV) and the focus actions from Niagara's viewport menu.
    ImGui::Checkbox("Grid",&document.showGrid); ImGui::SameLine();
    if (ImGui::Checkbox("Perspective",&document.perspective)) camera.Camera().SetProjectionMode(document.perspective ? Render::Settings::EProjectionMode::PERSPECTIVE : Render::Settings::EProjectionMode::ORTHOGRAPHIC);
    ImGui::SameLine(); ImGui::SetNextItemWidth(70);
    float fov = camera.Camera().GetFov();
    if (ImGui::DragFloat("FOV",&fov,0.5f,10.0f,140.0f,"%.0f")) camera.Camera().SetFov(std::clamp(fov,10.0f,140.0f));
    ImGui::SameLine(); if (ImGui::SmallButton("Focus")) FocusPreview(document,true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame the selected emitter (F)");
    ImGui::SameLine(); if (ImGui::SmallButton("Fit All")) FocusPreview(document,false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame every emitter");
    const auto available = ImGui::GetContentRegionAvail(); const ImVec2 size{(std::max)(1.0f,available.x),(std::max)(1.0f,available.y-25)};
    const auto origin = ImGui::GetCursorScreenPos();
    const auto width = static_cast<uint32_t>(std::clamp(size.x,1.0f,4096.0f));
    const auto height = static_cast<uint32_t>(std::clamp(size.y,1.0f,4096.0f));
    camera.HandleInputs({origin.x,origin.y},{origin.x+size.x,origin.y+size.y},ImGui::GetIO().DeltaTime,
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
    session.SetCameraContext(camera.MakeContext(width,height));
    document.previewHovered = false;
    if (session.Program() && NLS::Core::ServiceLocator::Contains<Render::Context::Driver>())
    {
        auto& driver = NLS::Core::ServiceLocator::Get<Render::Context::Driver>();
        VFXTools::VfxSystemPreviewInput input;
        input.width = width; input.height = height;
        input.isolatedEmitter = document.isolate ? session.selectedEmitter : std::string{};
        input.camera = camera.MakeView(width,height);
        input.gridEye = camera.Camera().GetPosition();
        const auto& gridColor = Settings::EditorSettings::GetViewSettingsObject().sceneViewGrid;
        input.gridColor = {gridColor.r,gridColor.g,gridColor.b};
        input.showGrid = document.showGrid;
        input.showOriginAxis = document.showOriginAxis;
        if (input.showGrid && NLS::Core::ServiceLocator::Contains<Core::Context>())
        {
            const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
            if (const auto* grid = context.editorResources->GetShader("Grid"))
            {
                const auto backend = Render::Context::DriverRendererAccess::GetExplicitDevice(driver)->GetAdapter()->GetBackendType();
                if (!document.gridShaders || document.gridShaders->generation != grid->GetGeneration() || document.gridShaders->vertex.targetBackend != backend)
                    document.gridShaders = Rendering::CaptureEditorGridShaders(*grid,backend);
            }
            input.gridShaders = document.gridShaders;
        }
        if (input.showOriginAxis && NLS::Core::ServiceLocator::Contains<Core::Context>())
        {
            auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
            if (const auto* shader = context.shaderManager[":Shaders/DebugPrimitive.hlsl"])
            {
                const auto device = Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
                const auto backend = device->GetAdapter()->GetBackendType();
                if (!document.axesShaders || document.axesShaders->generation != shader->GetGeneration() || document.axesShaders->vertex.targetBackend != backend)
                    document.axesShaders = Rendering::CaptureEditorGridShaders(*shader,backend);
            }
            input.axesShaders = document.axesShaders;
        }
        if (NLS::Core::ServiceLocator::Contains<Core::EditorActions>())
        {
            auto& panels = NLS::Core::ServiceLocator::Get<Core::EditorActions>().GetPanelsManager();
            if (panels.HasPanel("Asset Browser"))
                m_state->assets = panels.GetPanelAs<AssetBrowser>("Asset Browser").GetAssetDatabaseSnapshot();
        }
        const Engine::Rendering::SpriteAssetResolver resolve = [this](const NLS::VFX::AssetReference& reference,NLS::Core::Assets::ArtifactType type,std::string& path,std::string& error)
        {
            if (m_state->assets) for (const auto& item : m_state->assets->LoadAllAssetsAtPath(m_state->assets->GUIDToAssetPath(reference.guid)))
                if (item.artifactType == type && Engine::Serialize::MakeLocalIdentifierInFile(item.assetId.GetGuid(),item.subAssetKey) == reference.localId)
                { path = m_state->assets->ResolveArtifactPathAtPath(item.assetPath,item.subAssetKey).string(); if (!path.empty()) return true; }
            error = m_state->assets ? "Particle asset reference is missing or has the wrong type" : "Waiting for the project asset index..."; return false;
        };
        bool valid = true; std::string error;
        for (const auto& emitter : session.Program()->asset.emitters)
        {
            std::vector<NLS::VFX::SpriteMaterial> materials; std::vector<std::shared_ptr<const NLS::VFX::MeshRenderGeometry>> meshes;
            if (emitter.renderer == NLS::VFX::RendererType::Mesh && !Engine::Rendering::CaptureParticleMesh(emitter.mesh,meshes,error,resolve)) { valid = false; break; }
            for (size_t slot = 0; slot < (std::max)(size_t{1},meshes.size()); ++slot)
            {
                NLS::VFX::SpriteMaterial material;
                const auto settings = meshes.empty() ? emitter.sprite : NLS::VFX::ResolveMeshSlotMaterialSettings(emitter.sprite,emitter.mesh,slot);
                if (!Engine::Rendering::CaptureParticleSpriteMaterial(settings,material,error,resolve,session.Parameters())) { valid = false; break; }
                materials.push_back(std::move(material));
            }
            input.materials.push_back(std::move(materials)); input.meshes.push_back(std::move(meshes));
        }
        if (valid) document.preview.Update(Render::Context::DriverRendererAccess::GetExplicitDevice(driver),session.Program(),session.TakeFrames(),m_state->cacheRoot/"Shaders",std::move(input));
        else ImGui::TextWrapped("%s",error.c_str());
        const auto view = document.preview.View();
        auto* manager = NLS::Core::ServiceLocator::Contains<UI::UIManager>() ? &NLS::Core::ServiceLocator::Get<UI::UIManager>() : nullptr;
        void* texture = manager && view ? manager->ResolveTextureId(view) : nullptr;
        if (texture) { const bool flip = manager->ShouldFlipPresentedRenderTargetVertically(); ImGui::Image(texture,size,flip ? ImVec2{0,1} : ImVec2{0,0},flip ? ImVec2{1,0} : ImVec2{1,1}); }
        else ImGui::InvisibleButton("PreviewCanvas",size);
    }
    else
    { ImGui::InvisibleButton("PreviewCanvas",size); ImGui::GetWindowDrawList()->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(16,16,16,255)); }
    const bool hovered = ImGui::IsItemHovered();
    document.previewHovered = hovered;
    auto* draw = ImGui::GetWindowDrawList(); draw->PushClipRect(origin,{origin.x+size.x,origin.y+size.y},true);
    const auto viewProjection = camera.ViewProjection(width,height);
    const auto project = [&](const float x,const float y,const float z,ImVec2& out)
    {
        const float clipX = viewProjection[0]*x+viewProjection[1]*y+viewProjection[2]*z+viewProjection[3];
        const float clipY = viewProjection[4]*x+viewProjection[5]*y+viewProjection[6]*z+viewProjection[7];
        const float clipW = viewProjection[12]*x+viewProjection[13]*y+viewProjection[14]*z+viewProjection[15];
        if (clipW <= 1e-6f) return false;
        out = {origin.x+(clipX/clipW*0.5f+0.5f)*size.x,origin.y+(0.5f-clipY/clipW*0.5f)*size.y};
        return true;
    };
    if (document.showBounds)
        for (const auto& emitter : session.Instance().CpuEmitters()) if (emitter && !emitter->Bounds().Empty())
        {
            // CPU emitter bounds as a 3D box. GPU emitters only expose their
            // asynchronous ordering bounds to the scene path, so they stay out
            // of this overlay until the preview reads that snapshot back.
            const auto& bounds = emitter->Bounds();
            std::array<std::array<float,3>,8> corners{};
            for (int index = 0; index < 8; ++index)
            {
                const bool high = (index&1) != 0, highY = (index&2) != 0, highZ = (index&4) != 0;
                corners[index] = {high ? bounds.maximum[0]+bounds.spriteRadius : bounds.minimum[0]-bounds.spriteRadius,
                    highY ? bounds.maximum[1]+bounds.spriteRadius : bounds.minimum[1]-bounds.spriteRadius,
                    highZ ? bounds.maximum[2]+bounds.spriteRadius : bounds.minimum[2]-bounds.spriteRadius};
            }
            static constexpr int edges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
            for (const auto& edge : edges)
            {
                ImVec2 a,b;
                if (project(corners[edge[0]][0],corners[edge[0]][1],corners[edge[0]][2],a) &&
                    project(corners[edge[1]][0],corners[edge[1]][1],corners[edge[1]][2],b)) draw->AddLine(a,b,IM_COL32(255,220,70,220));
            }
        }
    if (document.showParticleCounts)
    {
        // Runtime overlay mirroring Niagara's particle count display. GPU
        // counts require the asynchronous counter readback and stay textual.
        const auto& instance = session.Instance(); const auto* program = session.Program().get();
        std::vector<std::string> rows; uint32_t total = 0;
        if (program) for (size_t i = 0; i < program->asset.emitters.size(); ++i)
        {
            const auto& emitter = program->asset.emitters[i];
            const bool cpu = i < instance.CpuEmitters().size() && instance.CpuEmitters()[i].has_value();
            const auto count = cpu ? instance.CpuEmitters()[i]->Data().Count() : 0u;
            total += count;
            rows.push_back("  "+emitter.name+(cpu ? "  "+std::to_string(count) : "  (GPU)"));
        }
        rows.insert(rows.begin(),"Particles "+std::to_string(total)+"   FPS "+std::to_string(static_cast<int>(ImGui::GetIO().Framerate+0.5f)));
        float width = 0; for (const auto& row : rows) width = (std::max)(width,ImGui::CalcTextSize(row.c_str()).x);
        const float pad = 4.0f, line = ImGui::GetTextLineHeightWithSpacing();
        draw->AddRectFilled(origin,{origin.x+width+2*pad,origin.y+line*static_cast<float>(rows.size())+pad},IM_COL32(0,0,0,120));
        for (size_t i = 0; i < rows.size(); ++i)
            draw->AddText({origin.x+pad,origin.y+pad+line*static_cast<float>(i)},i ? IM_COL32(210,210,210,235) : IM_COL32(255,225,140,255),rows[i].c_str());
    }
    draw->PopClipRect(); const auto status = document.preview.Status(); if (!status.empty()) ImGui::TextWrapped("%s",status.c_str());
}
bool VfxEditor::FocusPreview(Document& document,const bool selectedOnly)
{    auto& session = document.session; auto& camera = document.camera;
    const auto* program = session.Program().get();
    const auto& emitters = session.Instance().CpuEmitters();
    const bool filter = selectedOnly && !session.selectedEmitter.empty();
    std::array<float,3> minimum{1e30f,1e30f,1e30f}, maximum{-1e30f,-1e30f,-1e30f};
    bool any = false;
    for (size_t index = 0; index < emitters.size(); ++index)
    {
        if (!emitters[index]) continue;
        if (filter && (!program || index >= program->asset.emitters.size() || program->asset.emitters[index].id != session.selectedEmitter)) continue;
        const auto& bounds = emitters[index]->Bounds(); if (bounds.Empty()) continue;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            minimum[axis] = (std::min)(minimum[axis],bounds.minimum[axis]-bounds.spriteRadius);
            maximum[axis] = (std::max)(maximum[axis],bounds.maximum[axis]+bounds.spriteRadius);
        }
        any = true;
    }
    if (!any) { camera.ResetFocus(); return false; }
    std::array<float,3> center{}, extent{};
    for (size_t axis = 0; axis < 3; ++axis) { center[axis] = 0.5f*(minimum[axis]+maximum[axis]); extent[axis] = 0.5f*(maximum[axis]-minimum[axis]); }
    camera.Focus(center,std::sqrt(extent[0]*extent[0]+extent[1]*extent[1]+extent[2]*extent[2]));
    return true;
}
bool VfxEditor::OpenModuleSource(Document& document,const author::Module& module)
{
    auto& session = document.session;
    // Resolve the asset: an explicitly pinned script first, otherwise the
    // deterministic engine asset identity for a bare default module id.
    std::string assetId = module.scriptAsset.asset;
    if (assetId.empty() && !module.script.empty())
    {
        const auto engineId = author::EngineModuleAssetId(module.script);
        if (session.Library().Script(engineId)) assetId = engineId;
        else
            for (const auto& [id,script] : session.Library().Scripts())
                if (script.kind == author::AssetKind::Module && script.name == module.script) { assetId = id; break; }
    }
    if (assetId.empty()) return false;
    const auto dependency = session.Library().Dependency(assetId);
    if (!dependency || dependency->path.empty()) return false;
    EDITOR_PANEL(Editor::Panels::VfxLibraryEditor,"Particle Library Editor").OpenDocument(dependency->path);
    return true;
}

void VfxEditor::Timeline(Document& document)
{
    auto& session = document.session; auto& view = document.timeline;
    // Transport row.
    if (ImGui::Button("|<")) ExecuteCommand(&document,"Simulation.Reset"); ImGui::SameLine();
    if (ImGui::Button(session.playing ? "Pause" : "Play")) session.playing = !session.playing; ImGui::SameLine();
    if (ImGui::Button("<|")) ExecuteCommand(&document,"Simulation.StepBack"); ImGui::SameLine();
    if (ImGui::Button("|>")) ExecuteCommand(&document,"Simulation.Step"); ImGui::SameLine();
    int frameRate = static_cast<int>(document.frameRate); ImGui::SetNextItemWidth(58);
    if (ImGui::InputInt("FPS",&frameRate,0,0)) document.frameRate = static_cast<float>(std::clamp(frameRate,1,240));
    ImGui::SameLine(); float speed = static_cast<float>(session.playbackSpeed); ImGui::SetNextItemWidth(66);
    if (ImGui::DragFloat("Speed",&speed,0.05f,0,4,"%.2fx")) session.playbackSpeed = speed;
    ImGui::SameLine(); ImGui::Checkbox("Loop",&document.loopRange);
    ImGui::SameLine(); if (ImGui::SmallButton("Fit")) document.timeline.Fit(session.rangeEnd,1.0f);
    ImGui::SameLine(); ImGui::Text("%.3f / %.3f s",session.Instance().Age(),session.rangeEnd);
    if (session.IsSeeking()) { ImGui::SameLine(); ImGui::TextDisabled("Seeking..."); }
    // Ruler + tracks canvas.
    const auto available = ImGui::GetContentRegionAvail();
    const ImVec2 size{(std::max)(1.0f,available.x),(std::max)(90.0f,available.y-4.0f)};
    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    constexpr float rulerHeight = 20.0f, trackHeight = 18.0f;
    ImGui::InvisibleButton("##TimelineCanvas",size);
    const bool hovered = ImGui::IsItemHovered();
    const auto& io = ImGui::GetIO();
    const auto mouse = io.MousePos;
    const auto mouseTime = view.TimeAt(mouse.x-origin.x,size.x);
    const auto rangeX = view.XAt(session.rangeEnd,size.x);
    const bool onRangeHandle = std::fabs((mouse.x-origin.x)-rangeX) <= 5.0f && mouse.y <= origin.y+rulerHeight+4.0f;
    if (hovered && io.MouseWheel != 0.0f && !onRangeHandle)
    { view.ZoomAt(mouseTime,std::pow(1.1f,-io.MouseWheel)); }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) view.Pan(io.MouseDelta.x,size.x);
    // Rightmost handle trims the playback range.
    static bool draggingRange = false;
    if (hovered && onRangeHandle && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) draggingRange = true;
    if (draggingRange)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const auto snapped = VFXTools::VfxTimelineView::SnapTime((std::max)(0.05,mouseTime),0.05);
            session.rangeEnd = (std::min)(60.0,snapped);
        }
        else draggingRange = false;
    }
    // Scrubbing on the ruler (and anywhere else that is not the range handle).
    static bool scrubbing = false;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !onRangeHandle) { scrubbing = true; session.playing = false; }
    if (scrubbing)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) document.timelineSeek = static_cast<float>(view.ClampTime((std::max)(0.0,mouseTime)));
        else { scrubbing = false; session.RequestSeek(document.timelineSeek); }
    }
    if (!scrubbing && !session.IsSeeking()) document.timelineSeek = static_cast<float>(session.Instance().Age());
    // Background, range and ruler ticks.
    draw->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(24,24,24,255));
    draw->AddRectFilled({origin.x,origin.y},{origin.x+size.x,origin.y+rulerHeight},IM_COL32(34,34,34,255));
    draw->AddRectFilled({view.XAt(0.0,size.x)+origin.x,origin.y+rulerHeight},{view.XAt(session.rangeEnd,size.x)+origin.x,origin.y+size.y},IM_COL32(44,52,60,90));
    const auto pixelsPerSecond = (std::max)(size.x/(std::max)(1.0f,static_cast<float>(view.end-view.start)),1.0f);
    const double steps[] = {0.01,0.02,0.05,0.1,0.2,0.5,1.0,2.0,5.0,10.0};
    double tickStep = 10.0;
    for (const auto candidate : steps) if (candidate*static_cast<double>(pixelsPerSecond) >= 56.0) { tickStep = candidate; break; }
    const auto firstTick = std::floor(view.TimeAt(0.0f,size.x)/tickStep)*tickStep;
    for (auto tick = firstTick; ; tick += tickStep)
    {
        const auto x = origin.x+view.XAt(tick,size.x);
        if (x > origin.x+size.x) break;
        if (x < origin.x) continue;
        draw->AddLine({x,origin.y},{x,origin.y+size.y},IM_COL32(70,70,70,70));
        char label[32]{};
        if (tickStep < 1.0) std::snprintf(label,sizeof(label),"%.2f",tick); else std::snprintf(label,sizeof(label),"%.0f",tick);
        draw->AddText({x+2,origin.y+2},IM_COL32(170,170,170,220),label);
    }
    draw->AddLine({origin.x,origin.y+rulerHeight},{origin.x+size.x,origin.y+rulerHeight},IM_COL32(12,12,12,255));
    // Range handle.
    draw->AddLine({origin.x+rangeX,origin.y},{origin.x+rangeX,origin.y+size.y},IM_COL32(210,170,60,220),1.5f);
    draw->AddRectFilled({origin.x+rangeX-3,origin.y+2},{origin.x+rangeX+3,origin.y+12},IM_COL32(210,170,60,255),2.0f);
    // Playhead / scrub preview.
    const auto playhead = scrubbing ? document.timelineSeek : static_cast<float>(session.Instance().Age());
    const auto playheadX = origin.x+view.XAt(playhead,size.x);
    draw->AddLine({playheadX,origin.y},{playheadX,origin.y+size.y},IM_COL32(70,170,255,230),1.5f);
    draw->AddTriangleFilled({playheadX-5,origin.y},{playheadX+5,origin.y},{playheadX,origin.y+8},IM_COL32(70,170,255,255));
    // Emitter tracks: name, live particle count and a solo toggle that drives
    // the existing preview isolation.
    float trackY = origin.y+rulerHeight+4.0f;
    const auto& instance = session.Instance(); const auto* program = session.Program().get();
    for (size_t index = 0; index < instance.CpuEmitters().size() && trackY+trackHeight <= origin.y+size.y; ++index, trackY += trackHeight)
    {
        const auto name = program && index < program->asset.emitters.size() ? program->asset.emitters[index].name : "Emitter";
        const bool selected = program && index < program->asset.emitters.size() && program->asset.emitters[index].id == session.selectedEmitter;
        const bool cpu = instance.CpuEmitters()[index].has_value();
        const auto count = cpu ? instance.CpuEmitters()[index]->Data().Count() : 0u;
        draw->AddRectFilled({origin.x,trackY},{origin.x+size.x,trackY+trackHeight-1},selected ? IM_COL32(53,78,102,150) : IM_COL32(30,30,30,120));
        draw->AddRectFilled({origin.x+view.XAt(0.0,size.x),trackY},{origin.x+view.XAt(session.rangeEnd,size.x),trackY+trackHeight-2},IM_COL32(60,80,100,160));
        draw->AddText({origin.x+6,trackY+1},IM_COL32(220,220,220,240),name.c_str());
        const auto countText = cpu ? std::to_string(count)+" particles" : "GPU";
        draw->AddText({origin.x+size.x-120,trackY+1},IM_COL32(170,170,170,220),countText.c_str());
        ImGui::SetCursorScreenPos({origin.x+size.x-20,trackY});
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::InvisibleButton("##Solo",{16,trackHeight-2}))
        {
            document.isolate = !document.isolate;
            if (program && index < program->asset.emitters.size()) session.SelectEmitter(program->asset.emitters[index].id);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(document.isolate ? "Disable isolation" : "Isolate this emitter");
        draw->AddText({origin.x+size.x-15,trackY+1},selected ? IM_COL32(120,200,255,255) : IM_COL32(140,140,140,255),"S");
        ImGui::PopID();
    }
}

void VfxEditor::Diagnostics(Document& document)
{
    if (!document.session.document.Error().empty()) ImGui::TextWrapped("%s",document.session.document.Error().c_str());
    for (const auto& issue : document.session.Issues())
    {
        ImGui::PushID(&issue);
        if (ImGui::Selectable(issue.message.c_str())) document.session.SelectModule(issue.emitter,issue.stack,issue.module);
        if (ImGui::IsItemHovered() && !issue.input.empty()) ImGui::SetTooltip("Input: %s",issue.input.c_str());
        ImGui::PopID();
    }
}

void VfxEditor::Curves(Document& document)
{
    auto& session=document.session;
    auto candidate=session.document.Asset();
    auto* emitter=Selected(candidate,session);
    if(!emitter) {ImGui::TextUnformatted("Select a module with a curve input.");return;}
    for(auto& stack:emitter->stacks) for(auto& module:stack.modules) if(module.id==session.selectedModule)
        for(auto& [name,input]:module.inputs) if(input.mode==author::InputMode::Curve)
        {
            Properties::CurveValue value;
            value.channels=NLS::VFX::ComponentCount(input.type);
            for(const auto& key:input.curve)
                value.keys.push_back({key.time,{key.value[0],key.value[1],key.value[2],key.value[3]},
                    {key.arriveTangent[0],key.arriveTangent[1],key.arriveTangent[2],key.arriveTangent[3]},
                    {key.leaveTangent[0],key.leaveTangent[1],key.leaveTangent[2],key.leaveTangent[3]},static_cast<int32_t>(key.interpolation)});
            const auto edit=document.properties.DrawValue(module.id+":"+name+":curve",name,value);
            if(edit.changed)
            {
                input.curve.clear();
                for(const auto& key:value.keys) input.curve.push_back({static_cast<float>(key.time),
                    {key.value.x,key.value.y,key.value.z,key.value.w},
                    {key.inTangent.x,key.inTangent.y,key.inTangent.z,key.inTangent.w},
                    {key.outTangent.x,key.outTangent.y,key.outTangent.z,key.outTangent.w},
                    static_cast<author::CurveInterpolation>(key.interpolation)});
            }
            VFXTools::CommitInspectorEdit(session.document,candidate,edit,("Edit curve "+name).c_str());
        }
}

void VfxEditor::Script(Document& document)
{
    auto& session = document.session; auto candidate = session.document.Asset(); auto* emitter = Selected(candidate,session);
    NLS::VFX::Graph* graph = nullptr;
    if (emitter) for (auto& stack : emitter->stacks) for (auto& module : stack.modules) if (module.id == session.selectedModule)
        for (auto& [name,input] : module.inputs) if (input.mode == author::InputMode::DynamicInput && (!graph || name == document.search)) { graph = &input.expression; if (name == document.search) break; }
    if (!graph) { ImGui::TextWrapped("Choose Dynamic Input on a module input, then open its script."); return; }
    ImGui::TextUnformatted(document.search.c_str());
    if (ImGui::Button("+ Node")) ImGui::OpenPopup("CreateScriptNode");
    bool changed = false;
    if (ImGui::BeginPopup("CreateScriptNode"))
    {
        for (uint32_t op = 0; op <= static_cast<uint32_t>(NLS::VFX::Operation::Convert); ++op)
            if (op != static_cast<uint32_t>(NLS::VFX::Operation::WriteAttribute) && ((op != static_cast<uint32_t>(NLS::VFX::Operation::StageIndex) && op != static_cast<uint32_t>(NLS::VFX::Operation::StageIteration)) || graph->stage == NLS::VFX::ScriptStage::SimulationStage))
            if (ImGui::MenuItem(NLS::VFX::OperationName(static_cast<NLS::VFX::Operation>(op))))
            { NLS::VFX::AddNode(*graph,static_cast<NLS::VFX::Operation>(op)); changed = true; }
        ImGui::EndPopup();
    }
    ed::SetCurrentEditor(document.graph); ed::Begin("DynamicInputGraph",{0,0});
    if (document.graphRevision != session.document.Revision() || document.activeGraph != graph->id)
    {
        for (const auto& node : graph->nodes) ed::SetNodePosition(ed::NodeId(node.id),{node.position[0],node.position[1]});
        document.graphRevision = session.document.Revision(); document.activeGraph = graph->id;
    }
    for (auto& node : graph->nodes)
    {
        ImGui::PushID(static_cast<int>(node.id)); ed::BeginNode(ed::NodeId(node.id));
        ImGui::TextUnformatted(NLS::VFX::OperationName(node.operation));
        int type = static_cast<int>(node.type);
        ImGui::SetNextItemWidth(130);
        if (node.operation != NLS::VFX::Operation::WriteAttribute)
        {
            const auto edit = VFXTools::DrawVfxValueType(document.properties, "##Type", node.type, node.value);
            VFXTools::CommitInspectorEdit(session.document, candidate, edit, "Change script node type");
        }
        if (node.operation == NLS::VFX::Operation::Constant || node.operation == NLS::VFX::Operation::Random || node.operation == NLS::VFX::Operation::Parameter)
        {
            ImGui::SetNextItemWidth(150);
            Inspector::ReflectedPropertyOptions options;
            options.layout = Inspector::PropertyEditorLayout::ValueOnly;
            const auto edit = VFXTools::DrawVfxValue(document.properties, "##Value", node.type, node.value, options);
            VFXTools::CommitInspectorEdit(session.document, candidate, edit, "Edit dynamic input");
        }
        if (node.operation == NLS::VFX::Operation::Attribute || node.operation == NLS::VFX::Operation::Parameter)
        { ImGui::SetNextItemWidth(150); changed |= Text("##Name",node.name); }
        for (size_t i = 0; i < node.inputs.size(); ++i)
        { ed::BeginPin(ed::PinId((node.id<<4)+i),ed::PinKind::Input); ImGui::Text("o  %c",static_cast<char>('A'+i)); ed::EndPin(); }
        if (node.operation != NLS::VFX::Operation::WriteAttribute)
        { ed::BeginPin(ed::PinId((node.id<<4)+8),ed::PinKind::Output); ImGui::TextUnformatted("Value  o"); ed::EndPin(); }
        else ImGui::TextUnformatted("Output");
        ed::EndNode(); ImGui::PopID();
    }
    for (const auto& node : graph->nodes) for (size_t i = 0; i < node.inputs.size(); ++i) if (node.inputs[i])
        ed::Link(ed::LinkId((node.id<<4)+i),(ed::PinId((node.inputs[i]<<4)+8)),ed::PinId((node.id<<4)+i));
    if (ed::BeginCreate())
    {
        ed::PinId a,b;
        if (ed::QueryNewLink(&a,&b) && a && b)
        {
            if ((a.Get()&15) != 8) std::swap(a,b);
            const auto from = a.Get()>>4, to = b.Get()>>4; const auto slot = static_cast<uint32_t>(b.Get()&15);
            if ((a.Get()&15) == 8 && NLS::VFX::CanConnect(*graph,from,to,slot))
            { if (ed::AcceptNewItem()) { NLS::VFX::FindNode(*graph,to)->inputs[slot] = from; changed = true; } }
            else ed::RejectNewItem();
        }
    }
    ed::EndCreate();
    if (ed::BeginDelete())
    {
        ed::LinkId link;
        while (ed::QueryDeletedLink(&link)) if (ed::AcceptDeletedItem())
        { if (auto* node = NLS::VFX::FindNode(*graph,link.Get()>>4)) { const auto slot = link.Get()&15; if (slot < node->inputs.size()) { node->inputs[slot] = 0; changed = true; } } }
        ed::NodeId id;
        while (ed::QueryDeletedNode(&id))
        {
            const auto* node = NLS::VFX::FindNode(*graph,id.Get());
            if (node && node->operation != NLS::VFX::Operation::WriteAttribute && ed::AcceptDeletedItem())
            {
                std::erase_if(graph->nodes,[&](const auto& item) { return item.id == id.Get(); });
                for (auto& item : graph->nodes) for (auto& input : item.inputs) if (input == id.Get()) input = 0;
                changed = true;
            }
            else ed::RejectDeletedItem();
        }
    }
    ed::EndDelete();
    ed::End();
    const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    if (dragging && !document.graphEditing && ed::GetSelectedObjectCount() > 0)
    { session.document.BeginEdit("Move graph nodes"); document.graphEditing = true; }
    for (auto& node : graph->nodes)
    {
        const auto position = ed::GetNodePosition(ed::NodeId(node.id));
        if (node.position != std::array<float,2>{position.x,position.y}) { node.position = {position.x,position.y}; changed = true; }
    }
    ed::SetCurrentEditor(nullptr);
    if (changed)
    {
        if (document.graphEditing) session.document.PreviewEdit(std::move(candidate));
        else session.document.Apply(std::move(candidate),"Edit dynamic input graph");
    }
    if (document.graphEditing && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    { session.document.EndEdit(); document.graphEditing = false; }
}
}
