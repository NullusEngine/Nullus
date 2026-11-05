#pragma once

#include <gtest/gtest.h>
#include <Shortcuts/EditorShortcutAdapter.h>
#include <Shortcuts/EditorShortcutService.h>
#include <algorithm>
#include <stdexcept>

namespace NLS::Tests
{
// Test data builders only: evaluation, routing and dispatch always use the
// production InputSystem, compiled definitions and EditorShortcutAdapter.
struct TestShortcutCommand : Editor::Shortcuts::ShortcutCommand
{
    Editor::Shortcuts::ShortcutBinding defaultBinding;
    std::vector<Editor::Shortcuts::ShortcutBinding> alternateBindings;
};

class EditorInputMappingBuilder
{
public:
    Input::InputDefinition definition;
    EditorInputMappingBuilder() { definition.id = Guid::NewDeterministic("editor.input.test-definition"); }

    void SetContext(const Editor::Shortcuts::ShortcutContext& source)
    {
        auto& context = Context(source.id);
        context.priority = source.priority;
    }
    void Add(const TestShortcutCommand& command)
    {
        using namespace Editor::Shortcuts;
        const bool navigation = command.id.starts_with("scene-view.fly-");
        const auto actionId = navigation ? EditorShortcutAdapter::NavigationAction() : EditorShortcutAdapter::ActionForCommand(command.id);
        if (std::none_of(definition.actions.begin(), definition.actions.end(), [&](const auto& item) { return item.id == actionId; }))
        {
            Input::ActionDefinition action; action.id = actionId; action.name = command.id;
            if (navigation) { action.type = Input::ValueType::Vector3; action.nature = Input::ValueNature::Rate; action.aggregation = Input::Aggregation::Sum; }
            definition.actions.push_back(action);
        }
        const auto scope = command.id == "editor.release-game-input" ? "game-input-controls" :
            EditorShortcutAdapter::IsSessionControl(command.id) || command.allowDuringTextInput ? "editor-session-controls" : command.context;
        auto& context = Context(scope);
        auto defaults = std::vector<ShortcutBinding>{command.defaultBinding};
        defaults.insert(defaults.end(), command.alternateBindings.begin(), command.alternateBindings.end());
        for (size_t slot = 0; slot < defaults.size(); ++slot)
        {
            Input::BindingDefinition binding;
            binding.id = EditorShortcutAdapter::BindingForCommand(command.id, slot); binding.action = actionId;
            binding.chord = navigation ? EditorShortcutAdapter::NavigationChord(defaults[slot]) : EditorShortcutAdapter::ConvertChord(defaults[slot]);
            if (defaults[slot].IsValid()) binding.parts = {{{Input::DeviceKind::Keyboard, Input::ControlKind::Button, static_cast<uint16_t>(defaults[slot].primaryKey)}, {1}}};
            if (navigation)
            {
                binding.composite = Input::Composite::Vector3; binding.nature = Input::ValueNature::Rate;
                for (auto& part : binding.parts) part.contribution = EditorShortcutAdapter::NavigationContribution(command.id);
            }
            const auto found = std::find_if(context.bindings.begin(), context.bindings.end(), [&](const auto& item) { return item.id == binding.id; });
            if (found == context.bindings.end()) context.bindings.push_back(binding); else *found = binding;
        }
    }
    void AddAlias(const std::string& id, Editor::Shortcuts::ShortcutBinding alias)
    {
        using Editor::Shortcuts::EditorShortcutAdapter;
        const auto action = EditorShortcutAdapter::ActionForCommand(id);
        for (auto& context : definition.contexts)
        {
            if (std::none_of(context.bindings.begin(), context.bindings.end(), [&](const auto& value) { return value.action == action; })) continue;
            size_t count = std::count_if(context.bindings.begin(), context.bindings.end(), [&](const auto& value) { return value.action == action; });
            Input::BindingDefinition binding; binding.id = EditorShortcutAdapter::BindingForCommand(id, count); binding.action = action;
            binding.chord = EditorShortcutAdapter::ConvertChord(alias);
            if (alias.IsValid()) binding.parts = {{{Input::DeviceKind::Keyboard, Input::ControlKind::Button, static_cast<uint16_t>(alias.primaryKey)}, {1}}};
            context.bindings.push_back(binding);
            return;
        }
        throw std::logic_error("Missing test action: " + id);
    }
    void Apply(Editor::Shortcuts::EditorShortcutService& service) const
    {
        std::vector<std::string> diagnostics;
        auto compiled = Input::CompiledInput::Compile(definition, diagnostics);
        if (!compiled) throw std::logic_error(diagnostics.empty() ? "Invalid input test definition" : diagnostics.front());
        if (!service.SetInputDefaults(std::move(compiled))) throw std::logic_error("Input test service not initialized");
    }
private:
    Input::ContextDefinition& Context(const std::string& scope)
    {
        const auto id = Editor::Shortcuts::EditorShortcutAdapter::ContextForScope(scope);
        for (auto& context : definition.contexts) if (context.id == id) return context;
        Input::ContextDefinition context; context.id = id; context.name = scope;
        context.priority = scope == Editor::Shortcuts::ShortcutContexts::Global ? 0 : scope == Editor::Shortcuts::ShortcutContexts::SceneViewFlyMode ? 40 : scope == "editor-session-controls" ? 200 : scope == "game-input-controls" ? 210 : 20;
        context.reacquire = scope == Editor::Shortcuts::ShortcutContexts::SceneViewFlyMode ? Input::Reacquire::ResumeImmediately : Input::Reacquire::RequireRelease;
        definition.contexts.push_back(context);
        return definition.contexts.back();
    }
};

struct EditorInputTestHost
{
    double time = 0;
    Input::InputSystem input{[this] { return time; }};
};
class TestEditorShortcutService : private EditorInputTestHost, public Editor::Shortcuts::EditorShortcutService
{
public:
    using EditorInputTestHost::time;
    using EditorInputTestHost::input;
    EditorInputMappingBuilder mappings;
    TestEditorShortcutService() { InitializeInput(input); }
    bool RegisterCommand(TestShortcutCommand command)
    {
        const bool navigation = command.id.starts_with("scene-view.fly-");
        if (!navigation && !EditorShortcutService::RegisterCommand(command)) return false;
        mappings.Add(command); mappings.Apply(*this);
        return true;
    }
    void RegisterContext(Editor::Shortcuts::ShortcutContext context)
    {
        mappings.SetContext(context);
        EditorShortcutService::RegisterContext(std::move(context));
        mappings.Apply(*this);
    }
    void Key(uint16_t key, bool down, uint32_t modifiers = 0)
    {
        Input::InputEvent event; event.device = input.GetKeyboard(); event.control = key; event.modifiers = modifiers;
        event.type = down ? Input::InputEventType::ButtonDown : Input::InputEventType::ButtonUp;
        ASSERT_TRUE(input.SubmitEvent(event));
    }
    void Press(Editor::Shortcuts::ShortcutBinding binding, Editor::Shortcuts::EditorInputRoute route = {})
    {
        auto modifiers = static_cast<uint8_t>(Editor::Shortcuts::EditorShortcutAdapter::ConvertChord(binding).modifiers);
#ifdef __APPLE__
        if (modifiers & 16u) modifiers |= 8u;
#else
        if (modifiers & 16u) modifiers |= 2u;
#endif
        modifiers &= 15u;
        const uint16_t keys[] = {340, 341, 342, 343};
        for (unsigned bit = 0; bit < 4; ++bit) if (modifiers & (1u << bit)) Key(keys[bit], true, modifiers);
        Key(static_cast<uint16_t>(binding.primaryKey), true, modifiers);
        Key(static_cast<uint16_t>(binding.primaryKey), false, modifiers);
        for (unsigned bit = 0; bit < 4; ++bit) if (modifiers & (1u << bit)) Key(keys[bit], false, 0);
        UpdateInput(std::move(route));
        input.EndFrame(); time += .016;
    }
};
}
