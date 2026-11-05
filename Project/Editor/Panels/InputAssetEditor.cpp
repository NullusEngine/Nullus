#include "InputAssetEditor.h"
#include "Shortcuts/EditorShortcutService.h"
#include "Shortcuts/EditorCommandInvocation.h"
#include "Inputs/InputCodeGenerator.h"
#include <Core/Inputs/InputSerialization.h>
#include <Input/InputDomain.h>
#include <ServiceLocator.h>
#include <imgui.h>
#include <algorithm>
#include <array>

namespace NLS::Editor::Panels
{
namespace
{
bool Name(const char* label, std::string& value)
{
    std::array<char, 256> buffer{};
    std::copy_n(value.data(), std::min(value.size(), buffer.size() - 1), buffer.data());
    if (!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
    value = buffer.data(); return true;
}
template<class T> bool Enum(const char* label, T& value, const char* items)
{
    int selected = static_cast<int>(value);
    if (!ImGui::Combo(label, &selected, items)) return false;
    value = static_cast<T>(selected); return true;
}
bool Interaction(Input::Interaction& value)
{
    bool changed = Enum("Interaction", value.kind, "Press\0Release\0Hold\0Tap\0Multi Tap\0Repeat\0");
    changed |= ImGui::InputDouble("Duration", &value.duration);
    changed |= ImGui::InputDouble("Interval", &value.interval);
    int count = static_cast<int>(value.count);
    if (ImGui::InputInt("Tap count", &count)) { value.count = static_cast<uint32_t>(std::max(1, count)); changed = true; }
    return changed;
}
bool Processors(std::vector<Input::Processor>& processors)
{
    bool changed = false;
    if (!ImGui::TreeNode("Processors")) return false;
    for (size_t i = 0; i < processors.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        auto& processor = processors[i];
        changed |= Enum("Kind", processor.kind, "Scale\0Invert\0Deadzone\0Normalize\0Clamp\0");
        changed |= ImGui::DragFloat3("Scale", &processor.scale.x, .01f);
        changed |= ImGui::DragFloat("Minimum", &processor.minimum, .01f);
        changed |= ImGui::DragFloat("Maximum", &processor.maximum, .01f);
        if (ImGui::SmallButton("Remove")) { processors.erase(processors.begin() + i); changed = true; ImGui::PopID(); break; }
        ImGui::PopID(); ImGui::Separator();
    }
    if (ImGui::SmallButton("Add processor")) { processors.emplace_back(); changed = true; }
    ImGui::TreePop(); return changed;
}
bool Chord(Input::Chord& chord)
{
    bool changed = false;
    if (!ImGui::TreeNode("Chord")) return false;
    changed |= ImGui::Checkbox("Match modifiers", &chord.matchModifiers);
    changed |= Enum("Match", chord.modifierMatch, "Exact\0Required subset\0");
    int bits = static_cast<int>(chord.modifiers), forbidden = static_cast<int>(chord.forbiddenModifiers);
    if (ImGui::InputInt("Modifiers (Shift=1 Ctrl=2 Alt=4 Super=8 Primary=16)", &bits)) { chord.modifiers = static_cast<Input::Modifier>(std::clamp(bits, 0, 31)); changed = true; }
    if (ImGui::InputInt("Forbidden modifiers", &forbidden)) { chord.forbiddenModifiers = static_cast<Input::Modifier>(std::clamp(forbidden, 0, 31)); changed = true; }
    ImGui::TreePop(); return changed;
}
void User(Input::InputUser& user)
{
    const auto definition = user.GetDefinition();
    if (!definition) { ImGui::TextDisabled("No action asset"); return; }
    ImGui::PushID(static_cast<int>(user.GetId()));
    ImGui::Text("User %llu; epoch %llu; fixed tick %llu; pending %zu; enabled=%d suspended=%d",
        static_cast<unsigned long long>(user.GetId()), static_cast<unsigned long long>(user.GetEpoch()),
        static_cast<unsigned long long>(user.GetFixedFrame().tick), user.GetPendingFixedEventCount(), user.IsEnabled(), user.IsSuspended());
    for (const auto& context : definition->GetDefinition().contexts)
        ImGui::TextDisabled("Context %s: %s (priority %d)", context.name.c_str(), user.IsContextEnabled(context.id) ? "active" : "inactive", context.priority);
    for (const auto& action : definition->GetDefinition().actions)
    {
        const auto state = user.GetActionState(user.FindAction(action.id));
        ImGui::Text("%s: %.3f %.3f %.3f  pressed=%d released=%d performed=%d", action.name.c_str(),
            state.value.vector.x, state.value.vector.y, state.value.vector.z, state.pressed, state.released, state.performed);
    }
    for (const auto& id : user.GetUnappliedOverrides()) ImGui::TextColored({1,.6f,.2f,1}, "Unapplied BindingId: %s", id.ToString().c_str());
    if (ImGui::TreeNode("Ordered action events"))
    {
        for (const auto& event : user.GetActionEvents())
            ImGui::Text("%llu source=%llu tick=%llu phase=%u reason=%u time=%.6f action=%s",
                static_cast<unsigned long long>(event.sequence), static_cast<unsigned long long>(event.sourceSequence),
                static_cast<unsigned long long>(event.tick), unsigned(event.phase), unsigned(event.reason), event.realTimestamp, event.action.ToString().c_str());
        ImGui::TreePop();
    }
    ImGui::PopID();
}
}
InputAssetEditor::InputAssetEditor(const std::string& title, bool opened, const UI::PanelWindowSettings& settings)
    : PanelWindow(title, opened, settings) { minSize = {700, 500}; }
bool InputAssetEditor::OpenDocument(const std::filesystem::path& path)
{
    Open(); Focus();
    if (m_dirty && path != m_path) { m_diagnostics = {"Save the current input asset before opening another."}; return false; }
    Input::InputDefinition candidate; m_diagnostics.clear();
    if (!NLS::Core::Inputs::LoadInputDefinition(path, candidate, m_diagnostics)) return false;
    m_definition = std::move(candidate); m_path = path; m_dirty = false; return true;
}
bool InputAssetEditor::Save()
{
    m_diagnostics.clear();
    if (m_path.empty() || !Input::CompiledInput::Compile(m_definition, m_diagnostics)) return false;
    std::string error;
    if (!NLS::Core::Inputs::SaveInputDefinition(m_path, m_definition, error)) { m_diagnostics.push_back(error); return false; }
    auto generated = m_path; generated += ".g.cs";
    if (std::filesystem::exists(generated) && !Inputs::WriteInputCSharp(m_path, m_definition, error))
    { m_diagnostics.push_back(error); return false; }
    m_dirty = false; return true;
}
void InputAssetEditor::DrawDefinition()
{
    if (m_path.empty()) { ImGui::TextUnformatted("Open an .inputactions asset from the Asset Browser."); return; }
    ImGui::Text("%s%s", m_path.generic_string().c_str(), m_dirty ? " *" : "");
    if (ImGui::Button("Save"))
    {
        if (NLS::Core::ServiceLocator::Contains<Shortcuts::EditorShortcutService>()) Shortcuts::InvokeEditorCommand("input-editor.save", Shortcuts::CommandSource::Toolbar, "input-editor");
        else Save();
    }
    ImGui::SameLine(); if (ImGui::Button("Validate")) { m_diagnostics.clear(); (void)Input::CompiledInput::Compile(m_definition, m_diagnostics); }
    ImGui::SameLine();
    if (ImGui::Button("Generate C# Class"))
    {
        std::string error;
        if (Save() && !Inputs::WriteInputCSharp(m_path, m_definition, error)) m_diagnostics.push_back(error);
    }
    ImGui::TextDisabled("C# API: %s (generated beside the asset)", Inputs::InputCSharpClassName(m_path).c_str());
    if (ImGui::CollapsingHeader("Actions", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (auto& action : m_definition.actions)
        {
            ImGui::PushID(action.id.ToString().c_str());
            if (ImGui::TreeNode(action.name.c_str()))
            {
                ImGui::TextDisabled("%s", action.id.ToString().c_str());
                m_dirty |= Name("Name", action.name);
                m_dirty |= Enum("Value", action.type, "Button\0Axis\0Vector2\0Vector3\0");
                m_dirty |= Enum("Nature", action.nature, "State\0Delta\0Rate\0");
                m_dirty |= Enum("Aggregation", action.aggregation, "Maximum magnitude\0Sum\0");
                m_dirty |= ImGui::DragFloat("Press threshold", &action.pressThreshold, .01f);
                m_dirty |= ImGui::DragFloat("Release threshold", &action.releaseThreshold, .01f);
                m_dirty |= Processors(action.processors); m_dirty |= Chord(action.chord); m_dirty |= Interaction(action.interaction);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (ImGui::Button("Add action")) { Input::ActionDefinition action; action.id = Guid::New(); action.name = "New Action"; m_definition.actions.push_back(action); m_dirty = true; }
    }
    if (ImGui::CollapsingHeader("Contexts and Bindings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (auto& context : m_definition.contexts)
        {
            ImGui::PushID(context.id.ToString().c_str());
            if (ImGui::TreeNode(context.name.c_str()))
            {
                m_dirty |= Name("Name", context.name); m_dirty |= ImGui::InputInt("Priority", &context.priority);
                m_dirty |= Enum("Consumption", context.consumption, "Never\0On Actuated\0On Started\0On Performed\0");
                m_dirty |= Enum("Reactivation", context.reacquire, "Require release\0Resume immediately\0");
                if (ImGui::RadioButton("Default context", m_definition.defaultContext == context.id)) { m_definition.defaultContext = context.id; m_dirty = true; }
                for (size_t i = 0; i < context.bindings.size(); ++i)
                {
                    auto& binding = context.bindings[i]; ImGui::PushID(binding.id.ToString().c_str());
                    if (ImGui::TreeNode("Binding", "%s", binding.id.ToString().c_str()))
                    {
                        if (ImGui::BeginCombo("Action", binding.action.ToString().c_str()))
                        { for (const auto& action : m_definition.actions) if (ImGui::Selectable(action.name.c_str(), binding.action == action.id)) { binding.action = action.id; binding.nature = action.nature; m_dirty = true; } ImGui::EndCombo(); }
                        m_dirty |= Enum("Composite", binding.composite, "Direct\0Axis\0Vector2\0Vector3\0");
                        m_dirty |= Enum("Nature", binding.nature, "State\0Delta\0Rate\0");
                        for (size_t p = 0; p < binding.parts.size(); ++p)
                        {
                            ImGui::PushID(static_cast<int>(p)); auto& part = binding.parts[p];
                            m_dirty |= Enum("Device", part.path.device, "Keyboard\0Mouse\0Gamepad\0");
                            m_dirty |= Enum("Control", part.path.kind, "Button\0Axis\0Pointer position\0Pointer delta\0Wheel\0Scan code\0Logical key\0");
                            int control = part.path.control; if (ImGui::InputInt("Control index", &control)) { part.path.control = static_cast<uint16_t>(std::clamp(control, 0, 511)); m_dirty = true; }
                            if (part.path.kind == Input::ControlKind::ScanCode || part.path.kind == Input::ControlKind::LogicalKey)
                                m_dirty |= ImGui::InputScalar("Scan code / Unicode", ImGuiDataType_U32, &part.path.code, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
                            m_dirty |= ImGui::DragFloat3("Contribution", &part.contribution.vector.x, .1f);
                            if (ImGui::SmallButton("Remove control")) { binding.parts.erase(binding.parts.begin() + p); m_dirty = true; ImGui::PopID(); break; }
                            ImGui::PopID();
                        }
                        if (ImGui::SmallButton("Add control")) { binding.parts.push_back({{}, {1}}); m_dirty = true; }
                        m_dirty |= Processors(binding.processors); m_dirty |= Chord(binding.chord);
                        m_dirty |= ImGui::Checkbox("Binding interaction", &binding.hasInteraction);
                        if (binding.hasInteraction) m_dirty |= Interaction(binding.interaction);
                        if (ImGui::SmallButton("Remove binding")) { context.bindings.erase(context.bindings.begin() + i); m_dirty = true; ImGui::TreePop(); ImGui::PopID(); break; }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("Add binding") && !m_definition.actions.empty()) { Input::BindingDefinition binding; binding.id = Guid::New(); binding.action = m_definition.actions.front().id; binding.nature = m_definition.actions.front().nature; context.bindings.push_back(binding); m_dirty = true; }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (ImGui::Button("Add context")) { Input::ContextDefinition context; context.id = Guid::New(); context.name = "New Context"; m_definition.contexts.push_back(context); m_dirty = true; }
    }
}
void InputAssetEditor::DrawDebugger()
{
    if (NLS::Core::ServiceLocator::Contains<Shortcuts::EditorShortcutService>())
        if (auto* user = NLS::Core::ServiceLocator::Get<Shortcuts::EditorShortcutService>().GetInputUser()) User(*user);
    if (NLS::Core::ServiceLocator::Contains<Input::InputDomain>())
    {
        auto& domain = NLS::Core::ServiceLocator::Get<Input::InputDomain>();
        ImGui::Separator(); ImGui::TextUnformatted("Runtime users");
        for (const auto& user : domain.GetUsers()) User(*user);
        const auto& system = domain.GetInputSystem();
        for (const auto& device : system.GetDevices()) ImGui::Text("Device %llu: %s (%s)", static_cast<unsigned long long>(device.id.value), device.descriptor.name.c_str(), device.connected ? "connected" : "disconnected");
        ImGui::Text("Frame events: %zu; rejected: %llu", system.GetEvents().size(), static_cast<unsigned long long>(system.GetRejectedEventCount()));
    }
}
void InputAssetEditor::OnBeforeDrawWidgets()
{
    if (ImGui::BeginTabBar("Input"))
    {
        if (ImGui::BeginTabItem("Asset")) { DrawDefinition(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Debugger")) { DrawDebugger(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    for (const auto& diagnostic : m_diagnostics) ImGui::TextWrapped("%s", diagnostic.c_str());
}
}
