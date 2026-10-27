#include "VfxSession.h"
#include "VfxStackEditor.h"
#include <algorithm>
#include <cmath>

namespace NLS::Editor::VFXTools
{
void VfxSession::SetCameraContext(const NLS::VFX::CameraContext& camera)
{
    m_camera = camera;
    m_instance.SetCameraContext(m_camera);
}
void VfxSession::SetLibrary(NLS::VFX::Authoring::SourceLibrary library)
{
    const auto semantic = library.SemanticSource();
    if (semantic != m_librarySource || !m_libraryError.empty()) { m_librarySource = semantic; m_revision = 0; }
    if (!m_libraryError.empty()) m_attemptedSource.clear();
    m_libraryError.clear();
    m_library = std::move(library); m_editingRevision = 0;
}
void VfxSession::SetLibraryError(std::string error) { m_libraryError = std::move(error); m_revision = 0; }
bool VfxSession::Rebuild(bool force)
{
    ReconcileSelection();
    if (!m_libraryError.empty()) { m_issues = {{m_libraryError}}; return false; }
    if (!force && m_revision == document.Revision()) return m_issues.empty();
    if (!force && !autoCompile) return m_issues.empty();
    m_revision = document.Revision();
    const auto source = NLS::VFX::Authoring::SemanticSource(document.Asset())+m_librarySource;
    if (!force && source == m_attemptedSource) return m_issues.empty();
    m_attemptedSource = source;
    auto candidate = std::make_shared<NLS::VFX::CompiledSystem>();
    NLS::VFX::Authoring::System resolved; std::vector<NLS::VFX::Authoring::SourceDependency> dependencies;
    if (!NLS::VFX::Authoring::Compile(document.Asset(),m_library,*candidate,dependencies,m_issues,&resolved)) return false;
    NLS::VFX::SystemInstance instance; std::string error;
    if (!instance.Initialize(candidate,error)) { m_issues = {{error}}; return false; }
    instance.SetCameraContext(m_camera);
    std::unordered_map<std::string,NLS::VFX::Value> parameters, values;
    for (const auto& parameter : document.Asset().parameters)
    {
        const auto found = m_parameters.find(parameter.name);
        const auto value = found == m_parameters.end() ? parameter.value : found->second;
        values[parameter.name] = value;
        if (found != m_parameters.end()) parameters[parameter.name] = value;
    }
    if (!instance.SetParameters(values)) { m_issues = {{"User parameter overrides contain invalid Simulation Stage controls"}}; return false; }
    if (!instance.Tick(0)) { m_issues = {{"System warmup failed"}}; return false; }
    const double age = m_program && (!resetOnChange || (!playing && resimulatePaused)) ? m_instance.Age() : 0;
    // The UI and normal scene runtime use SystemInstance, including GPU frame
    // publication. Seeking advances fixed simulation frames, never a huge dt.
    std::vector<std::shared_ptr<const NLS::VFX::SystemFrame>> frames{instance.Capture()};
    m_program = std::move(candidate); m_instance = std::move(instance); m_parameters = std::move(parameters); ++m_generation;
    m_resolved = std::move(resolved); m_dependencies = std::move(dependencies);
    m_frames = std::move(frames);
    m_seekTarget = age > 0 ? std::optional<double>{age} : std::nullopt;
    m_seekElapsed = 0;
    return true;
}
bool VfxSession::Advance(double deltaTime)
{
    if (!m_program || !std::isfinite(deltaTime) || deltaTime <= 0 || deltaTime > 1) return false;
    if (m_seekTarget) return PumpSeek(8);
    const auto previous = m_instance.Capture();
    if (!m_instance.Tick(deltaTime)) return false;
    if (m_instance.Capture() != previous) m_frames.push_back(m_instance.Capture());
    return true;
}
bool VfxSession::Tick(double deltaTime)
{
    if (!m_program || !std::isfinite(deltaTime) || deltaTime < 0 || !std::isfinite(playbackSpeed) || playbackSpeed < 0) return false;
    if (m_seekTarget) return PumpSeek(8);
    if (!playing) return true;
    const auto previous = m_instance.Capture();
    if (!m_instance.Tick((std::min)(0.1,deltaTime)*playbackSpeed)) return false;
    if (m_instance.Capture() != previous) m_frames.push_back(m_instance.Capture()); return true;
}
bool VfxSession::Seek(double age)
{
    if (!RequestSeek(age)) return false;
    while (m_seekTarget) if (!PumpSeek(64)) return false;
    return true;
}
bool VfxSession::RequestSeek(double age)
{
    if (!m_program || !std::isfinite(age) || age < 0 || age > 600) return false;
    m_instance.Reset();
    for (const auto& [name,value] : m_parameters) m_instance.SetParameter(name,value);
    if (!m_instance.Tick(0)) return false;
    m_frames.clear(); m_frames.push_back(m_instance.Capture());
    m_seekElapsed = 0;
    m_seekTarget = age > 0 ? std::optional<double>{age} : std::nullopt; return true;
}
bool VfxSession::PumpSeek(uint32_t steps)
{
    for (uint32_t step = 0; step < steps && m_seekTarget; ++step)
    {
        const auto remaining = *m_seekTarget-m_seekElapsed;
        if (remaining <= 1e-9) { m_seekTarget.reset(); break; }
        const auto delta = (std::min)(remaining,1.0/60.0);
        const auto previous = m_instance.Capture();
        if (!m_instance.Tick(delta)) { m_seekTarget.reset(); return false; }
        m_seekElapsed += delta;
        if (m_instance.Capture() != previous) m_frames.push_back(m_instance.Capture());
    }
    return true;
}
void VfxSession::Reset() { if (m_program) { m_seekTarget.reset(); m_seekElapsed = 0; m_instance.Reset(); m_instance.Tick(0); m_frames.clear(); m_frames.push_back(m_instance.Capture()); } }
bool VfxSession::SetParameter(const std::string& name, NLS::VFX::Value value)
{
    if (!m_program || std::none_of(document.Asset().parameters.begin(),document.Asset().parameters.end(),[&](const auto& parameter) { return parameter.name == name; }) || !m_instance.SetParameter(name,value)) return false;
    m_parameters[name] = value; m_frames.push_back(m_instance.Capture()); return true;
}
const NLS::VFX::Authoring::System& VfxSession::EditingSystem()
{
    if (m_editingRevision != document.Revision() || m_editingLifetime.lock() != document.Lifetime().lock())
    {
        std::vector<NLS::VFX::Authoring::SourceDependency> dependencies;
        std::vector<NLS::VFX::Authoring::Issue> issues;
        if (!m_library.Resolve(document.Asset(),m_editingSystem,dependencies,issues)) m_editingSystem = document.Asset();
        m_editingRevision = document.Revision();
        m_editingLifetime = document.Lifetime();
    }
    return m_editingSystem;
}
void VfxSession::SelectSystem()
{
    if (selection != VfxSelection::System) document.EndEdit();
    selection = VfxSelection::System; selectedEmitter.clear(); selectedStack.clear(); selectedModule.clear();
    m_selectedEmitters.clear(); m_selectedModules.clear();
}
void VfxSession::SelectEmitter(std::string id)
{
    if (id.empty()) { SelectSystem(); return; }
    if (selection != VfxSelection::Emitter || selectedEmitter != id) document.EndEdit();
    selection = VfxSelection::Emitter; selectedEmitter = std::move(id); selectedStack.clear(); selectedModule.clear();
    m_selectedEmitters = {selectedEmitter}; m_selectedModules.clear();
}
void VfxSession::SelectEmitter(std::string id,const bool additive)
{
    if (!additive) { SelectEmitter(std::move(id)); return; }
    if (id.empty()) return;
    if (selection != VfxSelection::Emitter) document.EndEdit();
    selection = VfxSelection::Emitter; selectedEmitter = id; selectedStack.clear(); selectedModule.clear();
    m_selectedModules.clear();
    const auto found = std::find(m_selectedEmitters.begin(),m_selectedEmitters.end(),id);
    if (found == m_selectedEmitters.end()) m_selectedEmitters.push_back(id);
    else m_selectedEmitters.erase(found);
    if (m_selectedEmitters.empty()) m_selectedEmitters.push_back(selectedEmitter);
    else if (std::find(m_selectedEmitters.begin(),m_selectedEmitters.end(),selectedEmitter) == m_selectedEmitters.end())
        selectedEmitter = m_selectedEmitters.back();
}
void VfxSession::SelectStack(std::string emitter, std::string stack)
{
    if (selectedEmitter != emitter || selectedStack != stack || !selectedModule.empty()) document.EndEdit();
    for (const auto& view : EditingSystem().emitters) if (view.settings.id == emitter)
        for (const auto& candidate : view.stacks) if (candidate.id == stack || VfxSourceId(candidate) == stack)
        { stack = std::string(VfxSourceId(candidate)); break; }
    selection = VfxSelection::Stack; selectedEmitter = std::move(emitter); selectedStack = std::move(stack); selectedModule.clear();
    m_selectedEmitters = {selectedEmitter}; m_selectedModules.clear();
}
void VfxSession::SelectModule(std::string emitter, std::string stack, std::string module)
{
    if (selectedEmitter != emitter || selectedStack != stack || selectedModule != module) document.EndEdit();
    for (const auto& view : EditingSystem().emitters) if (view.settings.id == emitter)
        for (const auto& candidate : view.stacks) if (candidate.id == stack || VfxSourceId(candidate) == stack ||
            std::any_of(candidate.modules.begin(),candidate.modules.end(),[&](const auto& item) { return item.id == module || VfxSourceId(item) == module; }))
        {
            stack = std::string(VfxSourceId(candidate));
            for (const auto& item : candidate.modules) if (item.id == module || VfxSourceId(item) == module)
            { module = std::string(VfxSourceId(item)); break; }
            break;
        }
    selection = VfxSelection::Module; selectedEmitter = std::move(emitter); selectedStack = std::move(stack); selectedModule = std::move(module);
    m_selectedEmitters = {selectedEmitter}; m_selectedModules = {selectedModule};
}
void VfxSession::SelectModule(std::string emitter, std::string stack, std::string module,const bool additive)
{
    if (!additive) { SelectModule(std::move(emitter),std::move(stack),std::move(module)); return; }
    if (selectedEmitter != emitter || selectedStack != stack || selectedModule != module) document.EndEdit();
    for (const auto& view : EditingSystem().emitters) if (view.settings.id == emitter)
        for (const auto& candidate : view.stacks) if (candidate.id == stack || VfxSourceId(candidate) == stack ||
            std::any_of(candidate.modules.begin(),candidate.modules.end(),[&](const auto& item) { return item.id == module || VfxSourceId(item) == module; }))
        {
            stack = std::string(VfxSourceId(candidate));
            for (const auto& item : candidate.modules) if (item.id == module || VfxSourceId(item) == module)
            { module = std::string(VfxSourceId(item)); break; }
            break;
        }
    selection = VfxSelection::Module; selectedEmitter = std::move(emitter); selectedStack = std::move(stack); selectedModule = std::move(module);
    if (std::find(m_selectedEmitters.begin(),m_selectedEmitters.end(),selectedEmitter) == m_selectedEmitters.end())
        m_selectedEmitters.push_back(selectedEmitter);
    const auto found = std::find(m_selectedModules.begin(),m_selectedModules.end(),selectedModule);
    if (found == m_selectedModules.end()) m_selectedModules.push_back(selectedModule);
    else m_selectedModules.erase(found);
    if (m_selectedModules.empty()) m_selectedModules.push_back(selectedModule);
    else if (std::find(m_selectedModules.begin(),m_selectedModules.end(),selectedModule) == m_selectedModules.end())
        selectedModule = m_selectedModules.back();
}
void VfxSession::ClearSelection()
{
    m_selectedEmitters.clear(); m_selectedModules.clear();
    selectedStack.clear(); selectedModule.clear();
}
bool VfxSession::IsEmitterSelected(const std::string& emitterId) const
{
    return std::find(m_selectedEmitters.begin(),m_selectedEmitters.end(),emitterId) != m_selectedEmitters.end();
}
bool VfxSession::IsModuleSelected(const std::string& moduleId) const
{
    return std::find(m_selectedModules.begin(),m_selectedModules.end(),moduleId) != m_selectedModules.end();
}
void VfxSession::ReconcileSelection()
{
    const auto& system = EditingSystem();
    const auto validEmitter = [&](const std::string& id)
    { return std::any_of(system.emitters.begin(),system.emitters.end(),[&](const auto& item) { return item.settings.id == id; }); };
    const auto validModule = [&](const std::string& id)
    {
        for (const auto& emitter : system.emitters) for (const auto& stack : emitter.stacks)
            if (std::any_of(stack.modules.begin(),stack.modules.end(),[&](const auto& item) { return item.id == id || VfxSourceId(item) == id; })) return true;
        return false;
    };
    std::erase_if(m_selectedEmitters,[&](const auto& id) { return !validEmitter(id); });
    std::erase_if(m_selectedModules,[&](const auto& id) { return !validModule(id); });
    if (selection == VfxSelection::System) { m_selectedEmitters.clear(); m_selectedModules.clear(); return; }
    const auto emitter = std::find_if(system.emitters.begin(),system.emitters.end(),[&](const auto& item) { return item.settings.id == selectedEmitter; });
    if (emitter == system.emitters.end())
    {
        if (system.emitters.empty()) SelectSystem(); else SelectEmitter(system.emitters.front().settings.id);
        return;
    }
    if (m_selectedEmitters.empty()) m_selectedEmitters.push_back(selectedEmitter);
    if (selectedStack.empty()) return;
    const auto stack = std::find_if(emitter->stacks.begin(),emitter->stacks.end(),[&](const auto& item) { return item.id == selectedStack || VfxSourceId(item) == selectedStack; });
    if (stack == emitter->stacks.end()) { SelectEmitter(selectedEmitter); return; }
    if (!selectedModule.empty() && std::none_of(stack->modules.begin(),stack->modules.end(),[&](const auto& item) { return item.id == selectedModule || VfxSourceId(item) == selectedModule; })) SelectStack(selectedEmitter,selectedStack);
    if (m_selectedModules.empty() && !selectedModule.empty()) m_selectedModules.push_back(selectedModule);
}
bool VfxSession::SetModulesEnabled(const std::vector<std::string>& moduleIds,const bool enabled)
{
    if (moduleIds.empty()) return false;
    auto candidate = document.Asset(); bool found = false;
    for (auto& emitter : candidate.emitters) for (auto& stack : emitter.stacks) for (auto& module : stack.modules)
        if (std::find(moduleIds.begin(),moduleIds.end(),module.id) != moduleIds.end()) { module.enabled = enabled; found = true; }
    if (!found) return false;
    return document.Apply(std::move(candidate),enabled ? "Enable modules" : "Disable modules");
}
bool VfxSession::SetEmittersEnabled(const std::vector<std::string>& emitterIds,const bool enabled)
{
    if (emitterIds.empty()) return false;
    auto candidate = document.Asset(); bool found = false;
    for (auto& emitter : candidate.emitters)
        if (std::find(emitterIds.begin(),emitterIds.end(),emitter.settings.id) != emitterIds.end()) { emitter.enabled = enabled; found = true; }
    if (!found) return false;
    return document.Apply(std::move(candidate),enabled ? "Enable emitters" : "Disable emitters");
}
bool VfxSession::DeleteModules(const std::vector<std::string>& moduleIds)
{
    if (moduleIds.empty()) return false;
    auto candidate = document.Asset(); bool found = false;
    for (auto& emitter : candidate.emitters) for (auto& stack : emitter.stacks)
        found |= std::erase_if(stack.modules,[&](const auto& module) { return std::find(moduleIds.begin(),moduleIds.end(),module.id) != moduleIds.end(); }) > 0;
    if (!found) return false;
    if (!document.Apply(std::move(candidate),"Delete modules")) return false;
    m_selectedModules.clear(); ReconcileSelection();
    return true;
}
bool VfxSession::DeleteEmitters(const std::vector<std::string>& emitterIds)
{
    if (emitterIds.empty()) return false;
    auto candidate = document.Asset();
    const auto removed = std::erase_if(candidate.emitters,[&](const auto& emitter) { return std::find(emitterIds.begin(),emitterIds.end(),emitter.settings.id) != emitterIds.end(); });
    if (removed == 0) return false;
    if (!document.Apply(std::move(candidate),"Delete emitters")) return false;
    m_selectedEmitters.clear(); ReconcileSelection();
    return true;
}
std::vector<std::string> VfxSession::DuplicateModules(const std::vector<std::string>& moduleIds)
{
    std::vector<std::string> result;
    if (moduleIds.empty()) return result;
    auto candidate = document.Asset(); std::string error;
    for (const auto& id : moduleIds)
    {
        const auto copy = NLS::VFX::Authoring::DuplicateModule(candidate,id,error);
        if (copy.empty()) { m_actionError = error; return {}; }
        result.push_back(copy);
    }
    if (!document.Apply(std::move(candidate),"Duplicate modules")) return {};
    ReconcileSelection();
    return result;
}
void VfxSession::SelectModules(const std::vector<std::string>& moduleIds)
{
    if (moduleIds.empty()) return;
    auto candidate = document.Asset();
    for (auto& emitter : candidate.emitters) for (auto& stack : emitter.stacks) for (auto& module : stack.modules)
        if (module.id == moduleIds.front())
        {
            SelectModule(emitter.settings.id,stack.id,module.id);
            m_selectedModules = moduleIds;
            return;
        }
}
bool VfxSession::AddModule(const std::string& emitterId,const std::string& stackId,const std::string& script,bool asset)
{
    m_actionError.clear();
    const auto& view = EditingSystem();
    const NLS::VFX::Authoring::Stack* target = nullptr;
    for (const auto& emitter : view.emitters) if (emitter.settings.id == emitterId)
        for (const auto& stack : emitter.stacks) if (stack.id == stackId || VfxSourceId(stack) == stackId) target = &stack;
    if (!target) { m_actionError = "The emitter or stage is no longer available."; return false; }
    NLS::VFX::Authoring::Module module;
    if (asset)
    {
        const auto* source = m_library.Script(script);
        const auto* version = source ? NLS::VFX::Authoring::SelectVersion(*source,{}) : nullptr;
        if (!source || source->kind != NLS::VFX::Authoring::AssetKind::Module || !version ||
            std::find(version->stages.begin(),version->stages.end(),target->stage) == version->stages.end())
        { m_actionError = "This script is not a compatible module for this stage."; return false; }
        module = NLS::VFX::Authoring::InstantiateModule(*source);
    }
    else
    {
        const auto* definition = NLS::VFX::Authoring::FindModule(script);
        if (!definition || !NLS::VFX::Authoring::SupportsStage(*definition,target->stage))
        { m_actionError = "This module cannot run in this stage."; return false; }
        module = NLS::VFX::Authoring::CreateModule(script);
    }
    if (!document.EndEdit()) { m_actionError = document.Error(); return false; }
    auto candidate = document.Asset();
    auto emitter = std::find_if(candidate.emitters.begin(),candidate.emitters.end(),[&](const auto& item) { return item.settings.id == emitterId; });
    const auto sourceStack = std::string(VfxSourceId(*target));
    auto local = std::find_if(emitter->stacks.begin(),emitter->stacks.end(),[&](const auto& stack)
    { return stack.id == sourceStack || (target->stage != NLS::VFX::Authoring::Stage::Simulation && target->stage != NLS::VFX::Authoring::Stage::Event && stack.stage == target->stage); });
    if (local == emitter->stacks.end())
    {
        auto stack = *target; stack.id = sourceStack; stack.inheritedId.clear(); stack.modules.clear();
        emitter->stacks.push_back(std::move(stack)); local = std::prev(emitter->stacks.end());
    }
    const auto id = module.id; const auto localStack = local->id;
    local->modules.push_back(std::move(module));
    if (!document.Apply(std::move(candidate),"Add module")) { m_actionError = document.Error(); return false; }
    SelectModule(emitterId,localStack,id); return true;
}
bool VfxSession::SetModuleEnabled(const std::string& emitterId,const std::string& moduleId,bool enabled)
{
    m_actionError.clear();
    const NLS::VFX::Authoring::Module* target = nullptr;
    for (const auto& emitter : EditingSystem().emitters) if (emitter.settings.id == emitterId)
        for (const auto& stack : emitter.stacks) for (const auto& module : stack.modules)
            if (module.id == moduleId || VfxSourceId(module) == moduleId) target = &module;
    if (!target) { m_actionError = "The module is no longer available."; return false; }
    const auto id = std::string(VfxSourceId(*target));
    if (!document.EndEdit()) { m_actionError = document.Error(); return false; }
    auto candidate = document.Asset();
    for (auto& emitter : candidate.emitters) if (emitter.settings.id == emitterId)
    {
        bool local = false;
        for (auto& stack : emitter.stacks) for (auto& module : stack.modules) if (module.id == id) { module.enabled = enabled; local = true; }
        if (!local)
        {
            auto item = std::find_if(emitter.overrides.begin(),emitter.overrides.end(),[&](const auto& value) { return value.module == id; });
            if (item == emitter.overrides.end()) { emitter.overrides.push_back({id}); item = std::prev(emitter.overrides.end()); }
            item->enabled = enabled;
        }
    }
    if (!document.Apply(std::move(candidate),enabled ? "Enable module" : "Disable module")) { m_actionError = document.Error(); return false; }
    return true;
}
bool VfxSession::SetEmitterEnabled(const std::string& id,bool enabled)
{
    m_actionError.clear();
    if (!document.EndEdit()) { m_actionError = document.Error(); return false; }
    auto candidate = document.Asset();
    const auto emitter = std::find_if(candidate.emitters.begin(),candidate.emitters.end(),[&](const auto& value) { return value.settings.id == id; });
    if (emitter == candidate.emitters.end()) { m_actionError = "The emitter is no longer available."; return false; }
    emitter->enabled = enabled;
    if (!document.Apply(std::move(candidate),enabled ? "Enable emitter" : "Disable emitter")) { m_actionError = document.Error(); return false; }
    return true;
}
}
