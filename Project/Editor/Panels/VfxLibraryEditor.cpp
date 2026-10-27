#include "VfxLibraryEditor.h"
#include "VFX/Editor/VfxStageControlWidgets.h"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <imgui.h>
#include <imgui_node_editor.h>
#include <Guid.h>
#include <ServiceLocator.h>
#include <Windowing/Dialogs/OpenFileDialog.h>
#include <Windowing/Dialogs/SaveFileDialog.h>
#include "VFX/Editor/VfxLibraryDocument.h"
#include "VFX/Editor/VfxStyle.h"
#include "VFX/Editor/VfxInspector.h"
#include "Inspector/ReflectedEditHost.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Core/Context.h"

namespace NLS::Editor::Panels
{
namespace
{
namespace author = NLS::VFX::Authoring;
namespace ed = ax::NodeEditor;
using VFXTools::LibraryAsset;

class LibraryPropertyEditHost final : public Inspector::IReflectedEditHost
{
public:
    LibraryPropertyEditHost(VFXTools::VfxLibraryDocument& document, const LibraryAsset& asset)
        : m_document(document), m_asset(asset)
    {
    }

    [[nodiscard]] bool BeginPropertyEdit(const std::string_view label) override
    {
        return m_document.BeginEdit(std::string(label));
    }

    [[nodiscard]] bool PreviewPropertyEdit() override
    {
        if (m_document.PreviewEdit(m_asset))
        {
            m_error.clear();
            return true;
        }
        m_error = m_document.Error();
        return false;
    }

    [[nodiscard]] bool CommitPropertyEdit() override
    {
        if (m_document.EndEdit())
        {
            m_error.clear();
            return true;
        }
        m_error = m_document.Error();
        return false;
    }

    [[nodiscard]] bool CancelPropertyEdit() override
    {
        (void)m_document.EndEdit(false);
        return true;
    }

    [[nodiscard]] bool IsPropertyEditOpen() const override
    {
        return m_document.IsEditing();
    }

    [[nodiscard]] std::string_view GetPropertyEditError() const override
    {
        return m_error;
    }

private:
    VFXTools::VfxLibraryDocument& m_document;
    const LibraryAsset& m_asset;
    std::string m_error;
};


}
struct VfxLibraryEditor::State
{
    Undo::EditorUndoService undo;
    VFXTools::VfxLibraryDocument document{&undo};
    Properties::PropertyPanel properties;
    ed::EditorContext* graph = nullptr;
    std::string version,graphId,error;
    uint64_t revision = 0;
    std::unique_ptr<NLS::Dialogs::OpenFileDialog> open;
    std::unique_ptr<NLS::Dialogs::SaveFileDialog> save;
    std::filesystem::path pendingPath;
    std::optional<author::AssetKind> pendingKind;
    bool prompt = false,waitingToApply = false,graphEdit = false,pendingReadOnly = false;
    bool focused = false;
    author::SourceLibrary library;
    ~State() { if (graph) { ed::SetCurrentEditor(nullptr); ed::DestroyEditor(graph); } }
    void Commit(const LibraryAsset& asset,bool changed,const char* label)
    {
        if (ImGui::IsItemActivated()) document.BeginEdit(label);
        if (changed) { if (document.IsEditing()) document.PreviewEdit(asset); else document.Apply(asset,label); }
        if (ImGui::IsItemDeactivated() && document.IsEditing()) document.EndEdit();
    }
    void Commit(const LibraryAsset& asset,const Inspector::ReflectedPropertyEdit& edit,const char* label)
    {
        LibraryPropertyEditHost host(document, asset);
        if (!Inspector::ApplyReflectedEdit(host, edit, label))
        {
            error = host.GetPropertyEditError().empty()
                ? document.Error()
                : std::string(host.GetPropertyEditError());
        }
    }
};
VfxLibraryEditor::VfxLibraryEditor(const std::string& title,bool opened,const UI::PanelWindowSettings& settings)
    : PanelWindow(title,opened,settings),m_state(std::make_unique<State>()) { minSize = {900,650}; }
VfxLibraryEditor::~VfxLibraryEditor() = default;
bool VfxLibraryEditor::OwnsShortcuts() const { return IsOpened() && m_state->focused; }
void VfxLibraryEditor::ApplyPending()
{
    if (m_state->pendingKind) m_state->document.New(*m_state->pendingKind,*m_state->pendingKind == author::AssetKind::Emitter ? "Emitter" : "VFX Script");
    else if (!m_state->pendingPath.empty() && !m_state->document.Load(m_state->pendingPath,m_state->pendingReadOnly)) m_state->error = m_state->document.Error();
    m_state->pendingKind.reset(); m_state->pendingPath.clear(); m_state->pendingReadOnly = false; m_state->version.clear(); m_state->waitingToApply = false;
}
bool VfxLibraryEditor::OpenDocument(const std::filesystem::path& path)
{
    m_state->pendingPath = path; m_state->pendingKind.reset();
    m_state->pendingReadOnly = false;
    if (NLS::Core::ServiceLocator::Contains<Core::Context>())
        m_state->pendingReadOnly = author::IsPathWithin(path,NLS::Core::ServiceLocator::Get<Core::Context>().engineAssetsPath);
    const bool untouched = m_state->document.Path().empty() && m_state->document.Revision() == 1;
    if (m_state->document.IsDirty() && !untouched) m_state->prompt = true; else ApplyPending();
    Open(); Focus(); return true;
}
void VfxLibraryEditor::Save()
{
    if (m_state->document.Path().empty())
    {
        if (!m_state->save)
        {
            const bool emitter = m_state->document.Asset().kind == author::AssetKind::Emitter;
            m_state->save = std::make_unique<NLS::Dialogs::SaveFileDialog>("Save VFX Library Asset",emitter ? "Emitter.vfxemitter" : "Script.vfxscript",
                std::vector<std::string>{"VFX Library Asset",emitter ? "*.vfxemitter" : "*.vfxscript"},true);
        }
        return;
    }
    if (!m_state->document.Save()) m_state->error = m_state->document.Error();
    else { Publish(); if (m_state->waitingToApply) ApplyPending(); }
}
void VfxLibraryEditor::Publish()
{
    if (!NLS::Core::ServiceLocator::Contains<Core::Context>()) return;
    const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
    const auto path = std::filesystem::absolute(m_state->document.Path()).lexically_normal();
    const auto relative = path.lexically_relative(std::filesystem::absolute(context.projectAssetsPath).lexically_normal());
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return;
    const auto roots = Assets::MakeProjectEditorAssetRoots(std::filesystem::path(context.projectPath));
    Assets::AssetDatabaseFacade database(roots);
    if (!database.Refresh() || !database.ImportAsset(Assets::ToEditorAssetPath(roots,path)))
    { m_state->error = "The source was saved but could not be imported."; if (!database.GetDiagnostics().empty()) m_state->error += "\n"+database.GetDiagnostics().back().message; }
}
void VfxLibraryEditor::DuplicateToProject()
{
    auto& state = *m_state;
    if (!NLS::Core::ServiceLocator::Contains<Core::Context>()) { state.error = "Open a project before duplicating engine content."; return; }
    const auto asset = state.document.Asset();
    if (asset.kind != author::AssetKind::Module) { state.error = "Only module scripts can be duplicated into a project."; return; }
    auto copy = author::DuplicateScript(asset.script);
    const bool editable = std::any_of(copy.versions.begin(),copy.versions.end(),[](const auto& version) { return !version.graph.nodes.empty(); });
    if (!editable) { state.error = "This engine module has no graph; it cannot be duplicated into an editable script yet."; return; }
    const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
    const std::filesystem::path directory = std::filesystem::path(context.projectAssetsPath)/"VFX"/"Modules";
    std::error_code ignored; std::filesystem::create_directories(directory,ignored);
    const auto sanitize = [](std::string value)
    {
        for (auto& character : value)
            if (!std::isalnum(static_cast<unsigned char>(character)) && character != ' ' && character != '-' && character != '_')
                character = '_';
        return value;
    };
    auto path = directory/(sanitize(copy.name)+".vfxscript");
    for (int suffix = 2; std::filesystem::exists(path); ++suffix)
        path = directory/(sanitize(copy.name)+" "+std::to_string(suffix)+".vfxscript");
    std::string error;
    if (!author::SaveScript(path,copy,error)) { state.error = error; return; }
    state.error.clear();
    OpenDocument(path);
}
void VfxLibraryEditor::OnBeforeDrawWidgets()
{
    VFXTools::EnsureVfxInspectorReflection();
    m_state->properties.SetLifetime(m_state->document.Lifetime());
    VFXTools::VfxStyle style; auto& state = *m_state;
    state.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (state.focused && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))
    {
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S,false)) Save();
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z,false)) { state.document.EndEdit(); (void)state.undo.RequestUndo(); }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y,false)) { state.document.EndEdit(); (void)state.undo.RequestRedo(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape,false) && state.document.IsEditing()) { state.document.EndEdit(false); state.graphEdit = false; }
    }
    if (state.open && state.open->Ready(0)) { const auto paths = state.open->Result(); state.open.reset(); if (!paths.empty()) OpenDocument(paths.front()); }
    if (state.save && state.save->Ready(0))
    {
        auto path = std::filesystem::path(state.save->Result()); state.save.reset();
        if (!path.empty())
        {
            if (path.extension().empty()) path += state.document.Asset().kind == author::AssetKind::Emitter ? ".vfxemitter" : ".vfxscript";
            if (!state.document.Save(path)) state.error = state.document.Error(); else { Publish(); if (state.waitingToApply) ApplyPending(); }
        }
        else state.waitingToApply = false;
    }
    if (state.prompt) { state.prompt = false; ImGui::OpenPopup("Save VFX Library Asset?"); }
    if (ImGui::BeginPopupModal("Save VFX Library Asset?",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Save changes to the current library asset?");
        if (ImGui::Button("Save")) { state.waitingToApply = true; Save(); ImGui::CloseCurrentPopup(); } ImGui::SameLine();
        if (ImGui::Button("Discard")) { ApplyPending(); ImGui::CloseCurrentPopup(); } ImGui::SameLine();
        if (ImGui::Button("Cancel")) { state.pendingKind.reset(); state.pendingPath.clear(); ImGui::CloseCurrentPopup(); } ImGui::EndPopup();
    }
    if (ImGui::Button("New")) ImGui::OpenPopup("NewVfxLibraryAsset");
    if (ImGui::BeginPopup("NewVfxLibraryAsset"))
    {
        for (const auto [kind,name] : {std::pair{author::AssetKind::Module,"Module"},{author::AssetKind::Function,"Function"},{author::AssetKind::DynamicInput,"Dynamic Input"},{author::AssetKind::Emitter,"Emitter"}})
            if (ImGui::MenuItem(name)) { state.pendingKind = kind; state.pendingPath.clear(); if (state.document.IsDirty()) state.prompt = true; else ApplyPending(); }
        ImGui::EndPopup();
    }
    ImGui::SameLine(); if (ImGui::Button("Open") && !state.open) state.open = std::make_unique<NLS::Dialogs::OpenFileDialog>("Open VFX Library Asset","",std::vector<std::string>{"VFX Library Asset","*.vfxscript *.vfxemitter"});
    ImGui::SameLine(); if (ImGui::Button("Save")) Save(); ImGui::SameLine();
    ImGui::BeginDisabled(!state.undo.CanUndo()); if (ImGui::Button("Undo")) { state.document.EndEdit(); (void)state.undo.RequestUndo(); } ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!state.undo.CanRedo()); if (ImGui::Button("Redo")) { state.document.EndEdit(); (void)state.undo.RequestRedo(); } ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::Text("%s%s",state.document.Asset().Name().c_str(),state.document.IsDirty() ? " *" : "");
    if (state.document.IsReadOnly())
    {
        ImGui::SameLine(); ImGui::TextColored({0.95f,0.75f,0.25f,1},"Engine content (read-only)");
        ImGui::SameLine(); if (ImGui::Button("Duplicate to Project")) DuplicateToProject();
    }
    ImGui::SameLine(); if (ImGui::Button("Refresh Library") && NLS::Core::ServiceLocator::Contains<Core::Context>())
    {
        const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
        const std::array roots{std::filesystem::path(context.projectAssetsPath),std::filesystem::path(context.engineAssetsPath)};
        if (!state.library.Refresh(roots,state.error)) {} else state.error.clear();
    }
    auto candidate = state.document.Asset();
    ImGui::BeginDisabled(state.document.IsReadOnly());
    if (candidate.kind == author::AssetKind::Emitter)
    {
        auto& asset = candidate.emitter;
        if (state.version.empty()) state.version = asset.exposedVersion;
        if (ImGui::BeginCombo("Version",state.version.c_str()))
        { for (const auto& version : asset.versions) if (ImGui::Selectable((std::to_string(version.major)+"."+std::to_string(version.minor)).c_str(),version.id == state.version)) state.version = version.id; ImGui::EndCombo(); }
        if (ImGui::Button("New Version"))
        {
            if (const auto* selected = author::SelectVersion(asset,state.version))
            { auto copy = *selected; copy.id = Guid::New().ToString(); do { ++copy.minor; } while (std::any_of(asset.versions.begin(),asset.versions.end(),[&](const auto& value) { return value.major == copy.major && value.minor == copy.minor; })); state.version = copy.id; asset.versions.push_back(std::move(copy)); state.document.Apply(candidate,"Add emitter version"); }
        }
        ImGui::SameLine(); if (ImGui::Button("Expose Version")) { asset.exposedVersion = state.version; state.document.Apply(candidate,"Expose emitter version"); }
        EmitterProperties();
    }
    else
    {
        auto& asset = candidate.script;
        if (state.version.empty()) state.version = asset.exposedVersion;
        if (ImGui::BeginCombo("Version",state.version.c_str()))
        { for (const auto& version : asset.versions) if (ImGui::Selectable((std::to_string(version.major)+"."+std::to_string(version.minor)).c_str(),version.id == state.version)) state.version = version.id; ImGui::EndCombo(); }
        if (ImGui::Button("New Version"))
        {
            if (const auto* selected = author::SelectVersion(asset,state.version))
            { auto copy = *selected; copy.id = Guid::New().ToString(); copy.graph.id = Guid::New().ToString(); do { ++copy.minor; } while (std::any_of(asset.versions.begin(),asset.versions.end(),[&](const auto& value) { return value.major == copy.major && value.minor == copy.minor; })); state.version = copy.id; asset.versions.push_back(std::move(copy)); state.document.Apply(candidate,"Add script version"); }
        }
        ImGui::SameLine(); if (ImGui::Button("Expose Version")) { asset.exposedVersion = state.version; state.document.Apply(candidate,"Expose script version"); }
        const auto available = ImGui::GetContentRegionAvail(); ImGui::BeginChild("ScriptProperties",{310,available.y-30},true); ScriptProperties(); ImGui::EndChild(); ImGui::SameLine();
        ImGui::BeginChild("ScriptGraph",{0,available.y-30},true);
        if (state.document.IsReadOnly()) ImGui::TextDisabled("Engine content graph is read-only. Use Duplicate to Project to create an editable copy.");
        else DrawGraph();
        ImGui::EndChild();
    }
    ImGui::EndDisabled();
    if (!state.document.Error().empty()) ImGui::TextWrapped("%s",state.document.Error().c_str());
    if (!state.error.empty()) ImGui::TextWrapped("%s",state.error.c_str());
    if (state.properties.FinishFrame()) state.document.EndEdit();
}
void VfxLibraryEditor::ScriptProperties()
{
    auto& state = *m_state; auto candidate = state.document.Asset(); auto& asset = candidate.script;
    const auto found = std::find_if(asset.versions.begin(),asset.versions.end(),[&](const auto& value) { return value.id == state.version; });
    if (found == asset.versions.end()) return; auto& version = *found;
    const auto field = [&](const char* label,auto& value,const char* action)
    { const auto edit = state.properties.Draw(label,label,value); state.Commit(candidate,edit,action); };
    field("Name",asset.name,"Rename script");
    field("Category",asset.category,"Set script category");
    field("Visible in Library",asset.visible,"Set script visibility");
    field("Suggested",asset.suggested,"Set suggested script");
    field("Deprecated",asset.deprecated,"Deprecate script");
    field("Version Description",version.description,"Describe script version");
    for (const auto stage : {author::Stage::ParticleSpawn,author::Stage::ParticleUpdate,author::Stage::Simulation})
    {
        bool enabled = std::find(version.stages.begin(),version.stages.end(),stage) != version.stages.end();
        const auto edit = state.properties.Draw(author::StageName(stage),author::StageName(stage),enabled);
        if (edit.changed) { if (enabled) version.stages.push_back(stage); else std::erase(version.stages,stage); }
        state.Commit(candidate,edit,"Change script usage");
    }
    ImGui::SeparatorText("Inputs"); std::optional<size_t> removed;
    for (size_t i = 0; i < version.inputs.size(); ++i)
    {
        auto& input = version.inputs[i]; ImGui::PushID(input.id.c_str()); const auto previous = input.name;
        const auto nameEdit = state.properties.Draw("Name:"+input.id,"Name",input.name);
        if (nameEdit.changed)
        {
            for (auto& node : version.graph.nodes) if (node.operation == NLS::VFX::Operation::Parameter && node.name == "Module."+previous) node.name = "Module."+input.name;
            for (auto& branch : version.switches) if (branch.input == previous) branch.input = input.name;
        }
        state.Commit(candidate,nameEdit,"Rename script input");
        const auto previousType = input.type;
        const auto typeEdit = state.properties.Draw("Type:"+input.id,"Type",input.type);
        if (typeEdit.changed)
        {
            if (NLS::VFX::IsIntegralType(previousType) || NLS::VFX::IsIntegralType(input.type)) input.defaultValue = {};
            for (auto& node : version.graph.nodes) if (node.operation == NLS::VFX::Operation::Parameter && node.name == "Module."+input.name) { node.type = input.type; node.value = input.defaultValue; }
        }
        state.Commit(candidate,typeEdit,"Set script input type");
        const auto defaultEdit = VFXTools::DrawVfxValue(state.properties,"Default",input.type,input.defaultValue); state.Commit(candidate,defaultEdit,"Edit script default");
        const auto staticEdit = state.properties.Draw("Static:"+input.id,"Static",input.isStatic); state.Commit(candidate,staticEdit,"Set compile-time input");
        if (ImGui::Button("Add Read Node"))
        { const auto id = NLS::VFX::AddNode(version.graph,NLS::VFX::Operation::Parameter,input.type); auto* node = NLS::VFX::FindNode(version.graph,id); node->name = "Module."+input.name; node->value = input.defaultValue; state.document.Apply(candidate,"Add input node"); }
        ImGui::SameLine(); if (ImGui::Button("Delete")) removed = i; ImGui::Separator(); ImGui::PopID();
    }
    if (removed) { version.inputs.erase(version.inputs.begin()+*removed); state.document.Apply(candidate,"Delete script input"); }
    if (ImGui::Button("+ Input"))
    {
        std::string name; size_t i = version.inputs.size();
        do { name = "Input"+std::to_string(++i); } while (std::any_of(version.inputs.begin(),version.inputs.end(),[&](const auto& input) { return input.name == name; }));
        version.inputs.push_back({Guid::New().ToString(),name}); state.document.Apply(candidate,"Add script input");
    }
    ImGui::SeparatorText("Module Dependencies");
    std::string provides,requiresBefore;
    for (const auto& value : version.provides) { if (!provides.empty()) provides += ','; provides += value; }
    for (const auto& value : version.requiresBefore) { if (!requiresBefore.empty()) requiresBefore += ','; requiresBefore += value; }
    const auto split = [](const std::string& source)
    {
        std::vector<std::string> result; size_t first = 0;
        while (first < source.size()) { const auto last = source.find(',',first); const auto value = source.substr(first,last == std::string::npos ? last : last-first); if (!value.empty()) result.push_back(value); if (last == std::string::npos) break; first = last+1; } return result;
    };
    const auto providesEdit = state.properties.Draw("Provides","Provides",provides); if (providesEdit.changed) version.provides = split(provides); state.Commit(candidate,providesEdit,"Set module provided dependencies");
    const auto requiresEdit = state.properties.Draw("Requires Before","Requires Before",requiresBefore); if (requiresEdit.changed) version.requiresBefore = split(requiresBefore); state.Commit(candidate,requiresEdit,"Set module required dependencies");
}
void VfxLibraryEditor::EmitterProperties()
{
    auto& state = *m_state; auto candidate = state.document.Asset(); auto& asset = candidate.emitter;
    const auto found = std::find_if(asset.versions.begin(),asset.versions.end(),[&](const auto& value) { return value.id == state.version; });
    if (found == asset.versions.end()) return; auto& emitter = found->emitter;
    const auto nameEdit = state.properties.Draw("Name","Name",asset.name); state.Commit(candidate,nameEdit,"Rename emitter asset");
    const auto descriptionEdit = state.properties.Draw("Version Description","Version Description",found->description); state.Commit(candidate,descriptionEdit,"Describe emitter version");
    if (ImGui::BeginCombo("Parent",emitter.parent.Empty() ? "None" : emitter.parent.asset.c_str()))
    {
        if (ImGui::Selectable("None",emitter.parent.Empty())) { emitter.parent = {}; emitter.overrides.clear(); emitter.stackOverrides.clear(); state.document.Apply(candidate,"Remove emitter parent"); }
        for (const auto& [id,parent] : state.library.Emitters()) if (id != asset.id)
            if (ImGui::Selectable(parent.name.c_str(),id == emitter.parent.asset))
            { emitter.parent = {id,parent.exposedVersion}; emitter.stacks.clear(); emitter.overrides.clear(); emitter.stackOverrides.clear(); state.document.Apply(candidate,"Set emitter parent"); }
        ImGui::EndCombo();
    }
    const auto targetEdit = state.properties.Draw("Target","Sim Target",emitter.settings.target);
    if (targetEdit.changed && !emitter.parent.Empty() && std::find(emitter.settingsOverrides.begin(),emitter.settingsOverrides.end(),"target") == emitter.settingsOverrides.end()) emitter.settingsOverrides.push_back("target");
    state.Commit(candidate,targetEdit,"Set emitter target");
    if (!emitter.parent.Empty())
    {
        author::System wrapper; wrapper.id = asset.id; wrapper.name = asset.name; wrapper.emitters = {emitter};
        author::System resolved; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
        if (state.library.Resolve(wrapper,resolved,dependencies,issues)) for (const auto& stack : resolved.emitters[0].stacks)
            if (stack.stage == author::Stage::Simulation && std::none_of(emitter.stacks.begin(),emitter.stacks.end(),[&](const auto& value) { return value.id == stack.inheritedId; }))
            {
                ImGui::PushID(stack.id.c_str());
                if (ImGui::CollapsingHeader((stack.name+" (Inherited)").c_str(),ImGuiTreeNodeFlags_DefaultOpen))
                {
                    const auto stageOverride = [&]() -> author::Emitter::StackOverride&
                    {
                        for (auto& value : emitter.stackOverrides) if (value.stack == stack.inheritedId) return value;
                        emitter.stackOverrides.push_back({stack.inheritedId}); return emitter.stackOverrides.back();
                    };
                    VFXTools::VfxStageDetails fields{stack.enabled,stack.name,stack.iterations};
                    Inspector::ReflectedPropertyOptions options; if (!stack.bindings.iterations.empty()) options.disabled = {"iterations"};
                    const auto edit = state.properties.Draw("Stage:"+stack.id,"Simulation Stage",fields,options);
                    if (edit.Changed("enabled")) stageOverride().enabled = fields.enabled;
                    if (edit.Changed("name")) stageOverride().name = fields.name;
                    if (edit.Changed("iterations")) stageOverride().iterations = fields.iterations;
                    state.Commit(candidate,edit,"Override inherited stage properties");
                    auto bindings = stack.bindings;
                    VFXTools::DrawStageControlBindings(state.properties,bindings,{},true,[&](const auto& edit,const char* label)
                    { if (edit.changed) stageOverride().bindings = bindings; state.Commit(candidate,edit,label); });
                    if (std::any_of(emitter.stackOverrides.begin(),emitter.stackOverrides.end(),[&](const auto& value) { return value.stack == stack.inheritedId; }))
                        if (ImGui::Button("Reset Stage to Parent")) { std::erase_if(emitter.stackOverrides,[&](const auto& value) { return value.stack == stack.inheritedId; }); state.document.Apply(candidate,"Reset inherited stage"); }
                }
                ImGui::PopID();
            }
    }
    const auto stageCount = std::count_if(emitter.stacks.begin(),emitter.stacks.end(),[](const auto& stack) { return stack.stage == author::Stage::Simulation; });
    if (ImGui::Button("+ Simulation Stage") && stageCount < 64)
    { emitter.stacks.push_back(author::CreateSimulationStage()); state.document.Apply(candidate,"Add Simulation Stage"); }
    std::optional<author::Emitter> pending; std::string label; size_t stageIndex = 0;
    for (auto& stack : emitter.stacks)
    {
        const auto index = stageIndex; if (stack.stage == author::Stage::Simulation) ++stageIndex;
        ImGui::PushID(stack.id.c_str()); const bool expanded = ImGui::CollapsingHeader(stack.stage == author::Stage::Simulation ? stack.name.c_str() : author::StageName(stack.stage),ImGuiTreeNodeFlags_DefaultOpen);
        if (stack.stage == author::Stage::Simulation && ImGui::BeginPopupContextItem("StageActions"))
        {
            if (ImGui::MenuItem("Duplicate",nullptr,false,stageCount < 64))
            { auto copy = emitter; if (!author::DuplicateSimulationStage(copy,stack.id,state.error).empty()) { pending = std::move(copy); label = "Duplicate Simulation Stage"; } }
            const auto move = [&](size_t destination) { auto copy = emitter; if (author::MoveSimulationStage(copy,stack.id,destination,state.error)) { pending = std::move(copy); label = "Move Simulation Stage"; } };
            if (ImGui::MenuItem("Move Up",nullptr,false,index > 0)) move(index-1);
            if (ImGui::MenuItem("Move Down",nullptr,false,index+1 < stageCount)) move(index+1);
            if (ImGui::MenuItem("Delete")) { pending = emitter; std::erase_if(pending->stacks,[&](const auto& value) { return value.id == stack.id; }); label = "Delete Simulation Stage"; }
            ImGui::EndPopup();
        }
        if (expanded)
        {
            if (stack.stage == author::Stage::Simulation)
            {
                VFXTools::VfxStageDetails fields{stack.enabled,stack.name,stack.iterations};
                Inspector::ReflectedPropertyOptions options; if (!stack.bindings.iterations.empty()) options.disabled = {"iterations"};
                const auto edit = state.properties.Draw("Stage:"+stack.id,"Simulation Stage",fields,options);
                stack.enabled = fields.enabled; stack.name = fields.name; stack.iterations = fields.iterations;
                state.Commit(candidate,edit,"Edit Simulation Stage properties");
                VFXTools::DrawStageControlBindings(state.properties,stack.bindings,{},true,[&](const auto& edit,const char* label) { state.Commit(candidate,edit,label); });
                ImGui::TextDisabled("Iteration Source: Particles");
            }
            for (auto& module : stack.modules)
            {
                ImGui::PushID(module.id.c_str()); const auto* definition = author::FindModule(module.script);
                author::ModuleDefinition externalDefinition;
                if (const auto* source = state.library.Script(module.scriptAsset.asset)) if (const auto* version = author::SelectVersion(*source,module.scriptAsset.version))
                {
                    for (const auto& input : version->inputs) externalDefinition.inputs.push_back({input.name,input.type,input.defaultValue});
                    definition = &externalDefinition;
                    if (ImGui::BeginCombo("Version",module.scriptAsset.version.c_str()))
                    { for (const auto& version : source->versions) if (ImGui::Selectable((std::to_string(version.major)+"."+std::to_string(version.minor)).c_str(),version.id == module.scriptAsset.version)) { module.scriptAsset.version = version.id; state.document.Apply(candidate,"Change module script version"); } ImGui::EndCombo(); }
                }
                const auto enabledEdit = state.properties.Draw("Module:"+module.id,"Enabled",module.enabled); state.Commit(candidate,enabledEdit,"Enable emitter module");
                if (ImGui::BeginPopupContextItem("ModuleActions"))
                {
                    if (ImGui::MenuItem("Delete")) { pending = emitter; for (auto& target : pending->stacks) if (target.id == stack.id) std::erase_if(target.modules,[&](const auto& value) { return value.id == module.id; }); label = "Delete emitter module"; }
                    ImGui::EndPopup();
                }
                if (definition) for (const auto& inputDefinition : definition->inputs)
                {
                    auto& input = module.inputs.try_emplace(inputDefinition.name,author::Input{author::InputMode::Literal,inputDefinition.defaultValue}).first->second;
                    input.type = inputDefinition.type;
                    const auto edit = VFXTools::DrawVfxValue(state.properties,inputDefinition.name.c_str(),inputDefinition.type,input.value);
                    state.Commit(candidate,edit,"Edit emitter module input");
                }
                ImGui::PopID();
            }
            if (ImGui::Button("+ Add Module")) ImGui::OpenPopup("AddModule");
            if (ImGui::BeginPopup("AddModule"))
            {
                for (const auto& definition : author::ModuleLibrary()) if (author::SupportsStage(definition,stack.stage))
                    if (ImGui::MenuItem(definition.name.c_str())) { pending = emitter; for (auto& target : pending->stacks) if (target.id == stack.id) target.modules.push_back(author::CreateModule(definition.id)); label = "Add emitter module"; }
                for (const auto& [id,source] : state.library.Scripts()) if (source.kind == author::AssetKind::Module && source.visible)
                    if (const auto* version = author::SelectVersion(source,{}); version && std::find(version->stages.begin(),version->stages.end(),stack.stage) != version->stages.end())
                        if (ImGui::MenuItem(source.name.c_str())) { pending = emitter; for (auto& target : pending->stacks) if (target.id == stack.id) target.modules.push_back(author::InstantiateModule(source)); label = "Add referenced emitter module"; }
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }
    if (pending) { emitter = std::move(*pending); state.document.Apply(candidate,std::move(label)); }
}
void VfxLibraryEditor::DrawGraph()
{
    auto& state = *m_state; auto candidate = state.document.Asset(); auto& asset = candidate.script;
    const auto found = std::find_if(asset.versions.begin(),asset.versions.end(),[&](const auto& value) { return value.id == state.version; });
    if (found == asset.versions.end()) return; auto& version = *found; auto& graph = version.graph;
    if (!state.graph) { ed::Config config; config.SettingsFile = nullptr; state.graph = ed::CreateEditor(&config); }
    bool changed = false;
    if (ImGui::Button("+ Node")) ImGui::OpenPopup("NewLibraryNode");
    if (ImGui::BeginPopup("NewLibraryNode"))
    {
        for (uint32_t op = 0; op <= static_cast<uint32_t>(NLS::VFX::Operation::Convert); ++op)
            if ((op != static_cast<uint32_t>(NLS::VFX::Operation::StageIndex) && op != static_cast<uint32_t>(NLS::VFX::Operation::StageIteration)) || std::find(version.stages.begin(),version.stages.end(),author::Stage::Simulation) != version.stages.end())
            if (ImGui::MenuItem(NLS::VFX::OperationName(static_cast<NLS::VFX::Operation>(op))))
            { const auto id = NLS::VFX::AddNode(graph,static_cast<NLS::VFX::Operation>(op)); if (op == static_cast<uint32_t>(NLS::VFX::Operation::WriteAttribute)) NLS::VFX::FindNode(graph,id)->name = asset.kind == author::AssetKind::Module ? "Particles.Size" : "Output"; changed = true; }
        for (const auto& [id,function] : state.library.Scripts()) if (function.kind != author::AssetKind::Module && function.visible)
            if (ImGui::MenuItem(("Call "+function.name).c_str()))
            {
                const auto node = NLS::VFX::AddNode(graph,NLS::VFX::Operation::Parameter);
                version.calls.push_back({node,{id,function.exposedVersion}}); changed = true;
            }
        for (const auto& input : version.inputs) if (input.isStatic)
            if (ImGui::MenuItem(("Switch "+input.name).c_str()))
            {
                const auto yes = NLS::VFX::AddNode(graph,NLS::VFX::Operation::Constant), no = NLS::VFX::AddNode(graph,NLS::VFX::Operation::Constant);
                const auto node = NLS::VFX::AddNode(graph,NLS::VFX::Operation::Parameter);
                NLS::VFX::FindNode(graph,yes)->value[0] = 1; version.switches.push_back({node,yes,no,input.name}); changed = true;
            }
        ImGui::EndPopup();
    }
    ed::SetCurrentEditor(state.graph); ed::Begin("LibraryScriptGraph",{0,0});
    if (state.revision != state.document.Revision() || state.graphId != graph.id)
    {
        for (const auto& node : graph.nodes) ed::SetNodePosition(ed::NodeId(node.id),{node.position[0],node.position[1]});
        state.revision = state.document.Revision(); state.graphId = graph.id;
    }
    for (auto& node : graph.nodes)
    {
        ImGui::PushID(static_cast<int>(node.id)); ed::BeginNode(ed::NodeId(node.id));
        const auto call = std::find_if(version.calls.begin(),version.calls.end(),[&](const auto& value) { return value.node == node.id; });
        const auto branch = std::find_if(version.switches.begin(),version.switches.end(),[&](const auto& value) { return value.node == node.id; });
        ImGui::TextUnformatted(call != version.calls.end() ? "Function Call" : branch != version.switches.end() ? "Static Switch" : NLS::VFX::OperationName(node.operation));
        ImGui::SetNextItemWidth(130);
        const auto typeEdit = VFXTools::DrawVfxValueType(state.properties, "Type", node.type, node.value);
        state.Commit(candidate, typeEdit, "Change script node type");
        if (call != version.calls.end())
        {
            if (const auto* function = state.library.Script(call->script.asset))
            {
                ImGui::TextUnformatted(function->name.c_str());
                Inspector::ReflectedPropertyOptions versionOptions;versionOptions.layout=Inspector::PropertyEditorLayout::ValueOnly;
                for(const auto& version:function->versions) versionOptions.choices.push_back({version.id,std::to_string(version.major)+"."+std::to_string(version.minor)});
                const auto versionEdit=state.properties.DrawValue("Version","Version",call->script.version,versionOptions);
                state.Commit(candidate,versionEdit,"Change function version");
                if (const auto* selected = author::SelectVersion(*function,call->script.version)) for (const auto& input : selected->inputs)
                {
                    ImGui::PushID(input.name.c_str());
                    uint64_t argument=call->arguments.contains(input.name)?call->arguments[input.name]:0;
                    Inspector::ReflectedPropertyOptions argumentOptions;argumentOptions.layout=Inspector::PropertyEditorLayout::ValueOnly;
                    Inspector::InspectorPropertyChoice empty{"0","Default"};empty.typedValue=uint64_t{0};argumentOptions.choices.push_back(empty);
                    for(const auto& source:graph.nodes) if(source.id!=node.id && source.operation!=NLS::VFX::Operation::WriteAttribute)
                    {Inspector::InspectorPropertyChoice item{std::to_string(source.id),std::to_string(source.id)+" "+NLS::VFX::OperationName(source.operation)};item.typedValue=source.id;argumentOptions.choices.push_back(std::move(item));}
                    const auto argumentEdit=state.properties.DrawValue(input.name,input.name,argument,argumentOptions);
                    if(argumentEdit.changed) {if(argument) call->arguments[input.name]=argument;else call->arguments.erase(input.name);}
                    state.Commit(candidate,argumentEdit,"Change function argument");
                    ImGui::PopID();
                }
            }
        }
        else if (branch != version.switches.end())
        {
            ImGui::TextUnformatted(branch->input.c_str());
            for(bool yes:{true,false})
            {
                auto& sourceId=yes?branch->whenTrue:branch->whenFalse;
                Inspector::ReflectedPropertyOptions branchOptions;branchOptions.layout=Inspector::PropertyEditorLayout::ValueOnly;
                for(const auto& source:graph.nodes) if(source.id!=node.id && source.operation!=NLS::VFX::Operation::WriteAttribute)
                {Inspector::InspectorPropertyChoice item{std::to_string(source.id),std::to_string(source.id)};item.typedValue=source.id;branchOptions.choices.push_back(std::move(item));}
                const auto edit=state.properties.DrawValue(yes?"True":"False",yes?"True":"False",sourceId,branchOptions);
                state.Commit(candidate,edit,"Change switch branch");
            }
        }
        else
        {
            if (node.operation == NLS::VFX::Operation::Parameter || node.operation == NLS::VFX::Operation::Attribute || node.operation == NLS::VFX::Operation::WriteAttribute)
            {Inspector::ReflectedPropertyOptions options;options.layout=Inspector::PropertyEditorLayout::ValueOnly;const auto edit=state.properties.DrawValue("Name","Name",node.name,options);state.Commit(candidate,edit,"Rename script node");}
            if (node.operation == NLS::VFX::Operation::Constant || node.operation == NLS::VFX::Operation::Parameter || node.operation == NLS::VFX::Operation::Random)
            {
                ImGui::SetNextItemWidth(150);
                Inspector::ReflectedPropertyOptions options;
                options.layout = Inspector::PropertyEditorLayout::ValueOnly;
                const auto edit = VFXTools::DrawVfxValue(state.properties, "Value", node.type, node.value, options);
                state.Commit(candidate, edit, "Edit script node value");
            }
        }
        for (size_t i = 0; i < node.inputs.size(); ++i) { ed::BeginPin(ed::PinId((node.id<<4)+i),ed::PinKind::Input); ImGui::Text("o %c",static_cast<char>('A'+i)); ed::EndPin(); }
        if (node.operation != NLS::VFX::Operation::WriteAttribute) { ed::BeginPin(ed::PinId((node.id<<4)+8),ed::PinKind::Output); ImGui::TextUnformatted("Value o"); ed::EndPin(); }
        ed::EndNode(); ImGui::PopID();
    }
    for (const auto& node : graph.nodes) for (size_t i = 0; i < node.inputs.size(); ++i) if (node.inputs[i]) ed::Link(ed::LinkId((node.id<<4)+i),ed::PinId((node.inputs[i]<<4)+8),ed::PinId((node.id<<4)+i));
    if (ed::BeginCreate())
    {
        ed::PinId a,b;
        if (ed::QueryNewLink(&a,&b) && a && b)
        {
            if ((a.Get()&15) != 8) std::swap(a,b);
            const auto source = a.Get()>>4,destination = b.Get()>>4; const auto slot = static_cast<uint32_t>(b.Get()&15);
            if ((a.Get()&15) == 8 && NLS::VFX::CanConnect(graph,source,destination,slot)) { if (ed::AcceptNewItem()) { NLS::VFX::FindNode(graph,destination)->inputs[slot] = source; changed = true; } } else ed::RejectNewItem();
        }
    }
    ed::EndCreate();
    if (ed::BeginDelete())
    {
        ed::LinkId link; while (ed::QueryDeletedLink(&link)) if (ed::AcceptDeletedItem())
        { if (auto* node = NLS::VFX::FindNode(graph,link.Get()>>4); node && (link.Get()&15) < node->inputs.size()) { node->inputs[link.Get()&15] = 0; changed = true; } }
        ed::NodeId node;
        while (ed::QueryDeletedNode(&node))
        {
            const auto* selected = NLS::VFX::FindNode(graph,node.Get());
            if (selected && selected->operation != NLS::VFX::Operation::WriteAttribute && ed::AcceptDeletedItem())
            {
                std::erase_if(graph.nodes,[&](const auto& value) { return value.id == node.Get(); }); std::erase_if(version.calls,[&](const auto& value) { return value.node == node.Get(); }); std::erase_if(version.switches,[&](const auto& value) { return value.node == node.Get(); });
                for (auto& value : graph.nodes) for (auto& input : value.inputs) if (input == node.Get()) input = 0;
                for (auto& call : version.calls) for (auto& [name,input] : call.arguments) if (input == node.Get()) input = 0;
                for (auto& branch : version.switches) { if (branch.whenTrue == node.Get()) branch.whenTrue = 0; if (branch.whenFalse == node.Get()) branch.whenFalse = 0; }
                changed = true;
            }
            else ed::RejectDeletedItem();
        }
    }
    ed::EndDelete();
    ed::End();
    for (auto& node : graph.nodes)
    { const auto position = ed::GetNodePosition(ed::NodeId(node.id)); if (node.position != std::array<float,2>{position.x,position.y}) { node.position = {position.x,position.y}; changed = true; } }
    ed::SetCurrentEditor(nullptr);
    if (changed)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !state.graphEdit) { state.document.BeginEdit("Edit script graph"); state.graphEdit = true; }
        if (state.graphEdit) state.document.PreviewEdit(std::move(candidate)); else state.document.Apply(std::move(candidate),"Edit script graph");
    }
    if (state.graphEdit && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { state.document.EndEdit(); state.graphEdit = false; }
}
}
