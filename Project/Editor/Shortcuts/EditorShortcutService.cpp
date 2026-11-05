#include "Shortcuts/EditorShortcutService.h"
#include "EditorShortcutAdapter.h"
#include "EditorShortcutProfile.h"
#include <Input/InputRebinding.h>
#include <Core/Inputs/InputSerialization.h>
#include <Platform/Windowing/Inputs/InputDisplay.h>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace NLS::Editor::Shortcuts
{
    namespace { Input::BindingOverride ToInput(const std::string&, const ShortcutBinding&, size_t slot = 0); }
    EditorShortcutService::EditorShortcutService() = default;
    EditorShortcutService::~EditorShortcutService() = default;
    void EditorShortcutService::InitializeInput(Input::InputSystem& system)
    {
        m_adapter = std::make_unique<EditorShortcutAdapter>(system, m_registry);
        m_adapter->Rebuild(m_contexts);
    }
    bool EditorShortcutService::SetInputDefaults(std::shared_ptr<const Input::CompiledInput> definition)
    {
        if (!m_adapter || !definition) return false;
        m_adapter->SetDefaults(std::move(definition));
        m_adapter->Rebuild(m_contexts);
        return true;
    }
    const std::vector<Input::BindingId>& EditorShortcutService::GetBindingSlots(const std::string& id) const
    {
        static const std::vector<Input::BindingId> empty;
        return m_adapter && FindDescriptor(id) ? m_adapter->GetBindingSlots(id) : empty;
    }
    bool EditorShortcutService::LoadDefaults(const std::filesystem::path& path, std::vector<std::string>& diagnostics)
    {
        if (!m_adapter) return false;
        Input::InputDefinition definition;
        if (!NLS::Core::Inputs::LoadInputDefinition(path, definition, diagnostics)) return false;
        auto compiled = Input::CompiledInput::Compile(std::move(definition), diagnostics);
        if (!compiled) return false;
        SetInputDefaults(std::move(compiled));
        m_defaultsPath = path;
        std::error_code ec; m_defaultsTime = std::filesystem::last_write_time(path, ec);
        return true;
    }
    void EditorShortcutService::UpdateInput(EditorInputRoute route) { if (m_adapter) m_adapter->Update(m_contexts, route); }
    void EditorShortcutService::PrepareInput(EditorInputRoute route)
    {
        if (!m_adapter) return;
        const auto now = m_adapter->GetSystem().Now();
        if (!m_defaultsPath.empty() && now >= m_nextDefaultsPoll)
        {
            m_nextDefaultsPoll = now + .5;
            std::error_code ec; const auto stamp = std::filesystem::last_write_time(m_defaultsPath, ec);
            if (!ec && stamp != m_defaultsTime)
            { std::vector<std::string> diagnostics; LoadDefaults(m_defaultsPath, diagnostics); }
        }
        m_adapter->Prepare(m_contexts, route);
    }
    void EditorShortcutService::DispatchInput(EditorInputRoute route) { if (m_adapter) m_adapter->Dispatch(route); }
    Maths::Vector3 EditorShortcutService::GetNavigationValue() const
    {
        if (!m_adapter) return {};
        return m_adapter->GetNavigationValue().vector;
    }
    Input::InputUser* EditorShortcutService::GetInputUser() { return m_adapter ? &m_adapter->GetUser() : nullptr; }
    Input::ActionSnapshot EditorShortcutService::GetInputState(EditorInputAction action)
    { return m_adapter ? m_adapter->GetInputState(action) : Input::ActionSnapshot{}; }
    std::unique_ptr<Input::RebindingSession> EditorShortcutService::BeginRebinding(const std::string& id, size_t slot)
    {
        const auto* command = FindDescriptor(id);
        if (!m_adapter || !command || slot >= GetBindingSlots(id).size()) return {};
        Input::RebindingOptions options;
        return std::make_unique<Input::RebindingSession>(m_adapter->GetSystem(), m_adapter->GetUser(), GetBindingSlots(id)[slot], options);
    }
    bool EditorShortcutService::SetBindingSlot(const std::string& id, size_t slot, Input::BindingOverride value, std::string& error)
    {
        const auto* command = FindDescriptor(id);
        if (!m_adapter || !command || slot >= GetBindingSlots(id).size()) return false;
        if (command->requiredBinding && value.state == Input::OverrideState::ExplicitlyUnbound)
        { error = "This command requires a binding"; return false; }
        value.binding = GetBindingSlots(id)[slot];
        if (value.state != Input::OverrideState::ExplicitlyUnbound)
        {
            auto parts = value.parts;
            auto chord = value.chord.value_or(Input::Chord{});
            if (value.state == Input::OverrideState::Default)
                for (const auto& context : m_adapter->GetUser().GetDefinition()->GetDefinition().contexts)
                    for (const auto& binding : context.bindings) if (binding.id == value.binding) { parts = binding.parts; chord = binding.chord; }
            for (const auto& conflict : Input::FindBindingConflicts(m_adapter->GetUser(), value.binding, parts, chord,
                [this](auto first, auto second) { return AreInputContextsMutuallyExclusive(first, second); }))
            {
                const auto* other = FindBindingDescriptor(conflict.binding);
                if (other && AllowsBindingOverlap(*command, *other)) continue;
                error = "Conflicting binding: " + conflict.binding.ToString(); return false;
            }
        }
        m_adapter->GetUser().SetBindingOverride(std::move(value));
        return true;
    }
    std::string EditorShortcutService::GetBindingSlotDisplayText(const std::string& id, size_t slot) const
    {
        if (!m_adapter) return slot == 0 ? GetBindingDisplayText(id) : std::string{};
        const auto& slots = GetBindingSlots(id);
        return slot < slots.size() ? Windowing::Inputs::GetBindingDisplayName(m_adapter->GetUser(), slots[slot]) : std::string{};
    }
    CommandResult EditorShortcutService::TryExecuteCommand(const std::string& id, const InvocationContext& context)
    { return m_registry.TryExecuteCommand(id, context); }
    namespace
    {
        ShortcutBinding FromInput(const Input::InputUser& user, Input::BindingId id)
        {
            const auto parts = user.GetEffectiveBinding(id);
            if (parts.empty() || parts[0].path.device != Input::DeviceKind::Keyboard || parts[0].path.kind != Input::ControlKind::Button) return ShortcutBinding::Unassigned();
            const auto chord = user.GetEffectiveChord(id);
            ShortcutModifiers modifiers = 0;
            const auto bits = static_cast<uint8_t>(chord.modifiers);
            if (bits & 1u) modifiers |= ToModifierMask(EShortcutModifier::Shift);
            if (bits & 2u) modifiers |= ToModifierMask(EShortcutModifier::Ctrl);
            if (bits & 4u) modifiers |= ToModifierMask(EShortcutModifier::Alt);
            if (bits & 8u) modifiers |= ToModifierMask(EShortcutModifier::Super);
            if (bits & 16u) modifiers |= ToModifierMask(EShortcutModifier::Primary);
            return ShortcutBinding::FromKey(static_cast<Windowing::Inputs::EKey>(parts[0].path.control), modifiers);
        }
        Input::BindingOverride ToInput(const std::string& id, const ShortcutBinding& binding, size_t slot)
        {
            Input::BindingOverride result;
            result.binding = EditorShortcutAdapter::BindingForCommand(id, slot);
            result.state = binding.IsValid() ? Input::OverrideState::Override : Input::OverrideState::ExplicitlyUnbound;
            if (binding.IsValid())
            {
                result.parts = {{{Input::DeviceKind::Keyboard, Input::ControlKind::Button, static_cast<uint16_t>(binding.primaryKey)}, {1}}};
                result.chord = EditorShortcutAdapter::ConvertChord(binding);
                if (id.starts_with("scene-view.fly-"))
                {
                    result.chord = EditorShortcutAdapter::NavigationChord(binding);
                    result.parts[0].contribution = EditorShortcutAdapter::NavigationContribution(id);
                }
            }
            return result;
        }
    }

    bool EditorShortcutService::RegisterCommand(ShortcutCommand p_command)
    {
        return m_registry.RegisterCommand(std::move(p_command));
    }

    void EditorShortcutService::RegisterContext(ShortcutContext p_context)
    {
        m_contexts.emplace_back(std::move(p_context));
    }

    std::vector<ShortcutConflict> EditorShortcutService::ValidateBinding(
        const std::string& p_commandId,
        const ShortcutBinding& p_binding) const
    {
        const auto* target = FindDescriptor(p_commandId);
        const auto& slots = GetBindingSlots(p_commandId);
        if (!m_adapter || !target || slots.empty()) return {};
        if (!p_binding.IsValid()) return {{EShortcutConflictType::InvalidBinding, p_binding, {p_commandId}, "Shortcut binding is invalid.", true}};
        auto converted = ToInput(p_commandId, p_binding);
        converted.binding = slots.front();
        const auto collisions = Input::FindBindingConflicts(m_adapter->GetUser(), converted.binding, converted.parts, *converted.chord,
            [this](auto first, auto second) { return AreInputContextsMutuallyExclusive(first, second); });
        std::vector<ShortcutConflict> result;
        for (const auto& collision : collisions)
            for (const auto* other : GetCommands())
            {
                const auto& bindings = GetBindingSlots(other->id);
                if (std::find(bindings.begin(), bindings.end(), collision.binding) == bindings.end() || AllowsBindingOverlap(*target, *other)) continue;
                const auto type = target->context == other->context ? (target->context == ShortcutContexts::Global ? EShortcutConflictType::DuplicateGlobal : EShortcutConflictType::DuplicateContext) : EShortcutConflictType::GlobalContextCollision;
                result.push_back({type, p_binding, {p_commandId, other->id}, "Shortcut conflicts with " + other->displayName, true});
            }
        return result;
    }

    bool EditorShortcutService::LoadProfile(const std::filesystem::path& path, const std::filesystem::path& legacyPath)
    {
        if (!m_adapter || path.extension() != ".inputprofile") return false;
        m_profileDiagnostics.clear();
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec) return false;
        if (exists)
        {
            Input::InputProfile profile;
            if (!NLS::Core::Inputs::LoadInputProfile(path, profile, m_profileDiagnostics)) return false;
            m_adapter->GetUser().SetProfile(std::move(profile));
        }
        else
        {
            const auto previous = m_adapter->GetUser().GetProfile();
            if (!legacyPath.empty() && !ImportLegacyProfile(legacyPath)) return false;
            std::string error;
            if (!NLS::Core::Inputs::SaveInputProfile(path, m_adapter->GetUser().GetProfile(), error))
            {
                m_profileDiagnostics.push_back(error);
                m_adapter->GetUser().SetProfile(previous);
                return false;
            }
        }
        m_inputProfilePath = path;
        return true;
    }

    bool EditorShortcutService::SaveProfile(const std::filesystem::path& path) const
    {
        if (!m_adapter) return false;
        const auto& destination = path.empty() ? m_inputProfilePath : path;
        if (destination.empty() || destination.extension() != ".inputprofile") return false;
        std::string error;
        return NLS::Core::Inputs::SaveInputProfile(destination, m_adapter->GetUser().GetProfile(), error);
    }

    namespace
    {
        void StoreOverride(Input::InputProfile& profile, Input::BindingOverride value, bool resolved = true)
        {
            if (resolved) std::erase_if(profile.unresolved, [&](const auto& item) { return item.binding == value.binding; });
            const auto found = std::find_if(profile.overrides.begin(), profile.overrides.end(), [&](const auto& item) { return item.binding == value.binding; });
            if (found == profile.overrides.end()) profile.overrides.push_back(std::move(value)); else *found = std::move(value);
        }
    }

    bool EditorShortcutService::SetBinding(const std::string& id, const ShortcutBinding& binding)
    {
        const auto& slots = GetBindingSlots(id);
        if (!m_adapter || !FindDescriptor(id) || slots.empty() || !ValidateBinding(id, binding).empty()) return false;
        auto profile = m_adapter->GetUser().GetProfile();
        auto value = ToInput(id, binding); value.binding = slots.front();
        StoreOverride(profile, std::move(value));
        // Command-wide assignment keeps the legacy replacement-of-all-aliases
        // contract. Independent slots use SetBindingSlot instead.
        for (size_t slot = 1; slot < slots.size(); ++slot)
            StoreOverride(profile, {slots[slot], Input::OverrideState::ExplicitlyUnbound});
        m_adapter->GetUser().SetProfile(std::move(profile));
        return true;
    }

    bool EditorShortcutService::ClearBinding(const std::string& id)
    {
        const auto* descriptor = FindDescriptor(id);
        const auto& slots = GetBindingSlots(id);
        if (!m_adapter || !descriptor || descriptor->requiredBinding || slots.empty()) return false;
        auto profile = m_adapter->GetUser().GetProfile();
        for (auto binding : slots) StoreOverride(profile, {binding, Input::OverrideState::ExplicitlyUnbound});
        m_adapter->GetUser().SetProfile(std::move(profile));
        return true;
    }

    bool EditorShortcutService::ResetBinding(const std::string& id)
    {
        const auto& slots = GetBindingSlots(id);
        if (!m_adapter || slots.empty()) return false;
        // Validate the entire reset before committing any slot.
        const auto* descriptor = FindDescriptor(id);
        for (const auto& context : m_adapter->GetUser().GetDefinition()->GetDefinition().contexts)
            for (const auto& binding : context.bindings)
                if (std::find(slots.begin(), slots.end(), binding.id) != slots.end())
                    for (const auto& conflict : Input::FindBindingConflicts(m_adapter->GetUser(), binding.id, binding.parts, binding.chord,
                        [this](auto first, auto second) { return AreInputContextsMutuallyExclusive(first, second); }))
                    {
                        const auto* other = FindBindingDescriptor(conflict.binding);
                        if (!other || !AllowsBindingOverlap(*descriptor, *other)) return false;
                    }
        auto profile = m_adapter->GetUser().GetProfile();
        for (auto binding : slots)
        {
            std::erase_if(profile.overrides, [&](const auto& item) { return item.binding == binding; });
            std::erase_if(profile.unresolved, [&](const auto& item) { return item.binding == binding; });
        }
        m_adapter->GetUser().SetProfile(std::move(profile));
        return true;
    }

    std::vector<const ShortcutDescriptor*> EditorShortcutService::SearchCommands(const std::string& p_query) const
    {
        auto normalize = [](std::string value)
        {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](unsigned char ch)
                {
                    return static_cast<char>(std::tolower(ch));
                });
            return value;
        };

        const auto query = normalize(p_query);
        std::vector<const ShortcutDescriptor*> result;
        for (const auto* descriptor : GetCommands())
        {
            const auto& command = *descriptor;
            const auto haystack = normalize(
                command.id + " " +
                command.displayName + " " +
                command.category + " " +
                command.context + " " +
                GetBindingDisplayText(command.id));

            if (query.empty() || haystack.find(query) != std::string::npos)
                result.push_back(&command);
        }

        return result;
    }

    std::vector<const ShortcutDescriptor*> EditorShortcutService::GetCommands() const
    {
        std::vector<const ShortcutDescriptor*> result;
        for (const auto& command : m_registry.GetCommands()) result.push_back(&command);
        for (const auto& setting : EditorShortcutAdapter::NavigationSettings())
            if (!GetBindingSlots(setting.id).empty()) result.push_back(&setting);
        for (const auto& setting : EditorShortcutAdapter::ContinuousSettings())
            if (!m_registry.FindCommand(setting.id) && !GetBindingSlots(setting.id).empty()) result.push_back(&setting);
        return result;
    }

    const ShortcutDescriptor* EditorShortcutService::FindDescriptor(const std::string& id) const
    {
        if (const auto* command = m_registry.FindCommand(id)) return command;
        for (const auto& setting : EditorShortcutAdapter::NavigationSettings()) if (setting.id == id) return &setting;
        for (const auto& setting : EditorShortcutAdapter::ContinuousSettings()) if (setting.id == id) return &setting;
        return nullptr;
    }

    bool EditorShortcutService::AllowsBindingOverlap(const ShortcutDescriptor& first, const ShortcutDescriptor& second) const
    {
        return first.context != second.context &&
            ((first.context != ShortcutContexts::Global && second.context != ShortcutContexts::Global) ||
             first.allowContextOverride || second.allowContextOverride);
    }

    const ShortcutDescriptor* EditorShortcutService::FindBindingDescriptor(Input::BindingId binding) const
    {
        for (const auto* descriptor : GetCommands())
        {
            const auto& slots = GetBindingSlots(descriptor->id);
            if (std::find(slots.begin(), slots.end(), binding) != slots.end()) return descriptor;
        }
        return nullptr;
    }

    bool EditorShortcutService::ImportLegacyProfile(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return !ec;
        EditorShortcutProfile legacy;
        if (!legacy.Load(path)) { m_profileDiagnostics.push_back("Could not import " + path.string()); return false; }
        auto& user = m_adapter->GetUser();
        auto profile = user.GetProfile();
        // Convert all proposals first, so two commands exchanging keys do not
        // spuriously conflict with the defaults they are both replacing.
        std::vector<std::string> ids;
        for (const auto& [id, _] : legacy.GetBindingOverrides()) ids.push_back(id);
        for (const auto& id : legacy.GetUnassignedCommands())
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        if (ids.empty()) return true;
        for (const auto& id : ids)
        {
            const auto& slots = GetBindingSlots(id);
            const auto* descriptor = FindDescriptor(id);
            const bool unassigned = legacy.IsCommandUnassigned(id);
            const auto binding = legacy.GetBindingOverride(id).value_or(ShortcutBinding::Unassigned());
            if (!descriptor || slots.empty() || (unassigned && descriptor->requiredBinding))
            {
                profile.unresolved.push_back(ToInput(id, unassigned ? ShortcutBinding::Unassigned() : binding));
                m_profileDiagnostics.push_back("Unapplied legacy shortcut: " + id);
                continue;
            }
            auto value = ToInput(id, unassigned ? ShortcutBinding::Unassigned() : binding);
            value.binding = slots.front(); StoreOverride(profile, std::move(value));
            for (size_t slot = 1; slot < slots.size(); ++slot)
                StoreOverride(profile, {slots[slot], Input::OverrideState::ExplicitlyUnbound});
        }
        user.SetProfile(profile);

        struct Candidate { const ShortcutDescriptor* descriptor; Input::BindingId binding; int priority; };
        std::vector<Candidate> candidates;
        for (const auto* descriptor : GetCommands())
            for (auto id : GetBindingSlots(descriptor->id))
                for (const auto& context : user.GetDefinition()->GetDefinition().contexts)
                    for (const auto& binding : context.bindings)
                        if (binding.id == id) candidates.push_back({descriptor, id, context.priority});
        // Legacy precedence: context priority, then command registration order.
        // Never depend on JSON object or unordered_map iteration order.
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.priority > b.priority; });
        std::unordered_set<Input::BindingId> retained;
        for (const auto& candidate : candidates)
        {
            const auto parts = user.GetEffectiveBinding(candidate.binding);
            if (parts.empty()) continue;
            const auto conflicts = Input::FindBindingConflicts(user, candidate.binding, parts, user.GetEffectiveChord(candidate.binding),
                [this](auto a, auto b) { return AreInputContextsMutuallyExclusive(a, b); });
            const auto conflict = std::find_if(conflicts.begin(), conflicts.end(), [&](const auto& other)
            {
                const auto* descriptor = FindBindingDescriptor(other.binding);
                return retained.contains(other.binding) && (!descriptor || !AllowsBindingOverlap(*candidate.descriptor, *descriptor));
            });
            if (conflict == conflicts.end()) { retained.insert(candidate.binding); continue; }
            const auto existing = std::find_if(profile.overrides.begin(), profile.overrides.end(),
                [&](const auto& value) { return value.binding == candidate.binding; });
            profile.unresolved.push_back(existing == profile.overrides.end() ? Input::BindingOverride{candidate.binding} : *existing);
            StoreOverride(profile, {candidate.binding, Input::OverrideState::ExplicitlyUnbound}, false);
            m_profileDiagnostics.push_back("Conflicting legacy shortcut disabled: " + candidate.descriptor->id + " (" + candidate.binding.ToString() + ")");
            user.SetProfile(profile);
        }
        return true;
    }

    std::string EditorShortcutService::GetBindingDisplayText(const std::string& id) const
    {
        std::string result;
        if (m_adapter) for (auto binding : GetBindingSlots(id))
        {
            if (m_adapter->GetUser().GetEffectiveBinding(binding).empty()) continue;
            if (!result.empty()) result += " / ";
            result += Windowing::Inputs::GetBindingDisplayName(m_adapter->GetUser(), binding);
        }
        return result.empty() ? "Unassigned" : result;
    }

    ShortcutBinding EditorShortcutService::GetBinding(const std::string& id) const
    {
        const auto& slots = GetBindingSlots(id);
        return m_adapter && !slots.empty() ? FromInput(m_adapter->GetUser(), slots.front()) : ShortcutBinding::Unassigned();
    }

    std::string EditorShortcutService::CaptureBindingState(const std::string& id) const
    {
        const auto& slots = GetBindingSlots(id);
        if (!m_adapter || slots.empty()) return {};
        Input::InputProfile snapshot;
        const auto& profile = m_adapter->GetUser().GetProfile();
        for (auto binding : slots)
        {
            const auto found = std::find_if(profile.overrides.begin(), profile.overrides.end(), [&](const auto& item) { return item.binding == binding; });
            snapshot.overrides.push_back(found == profile.overrides.end() ? Input::BindingOverride{binding} : *found);
            for (const auto& item : profile.unresolved) if (item.binding == binding) snapshot.unresolved.push_back(item);
        }
        return NLS::Core::Inputs::SerializeInputProfile(snapshot);
    }
    bool EditorShortcutService::RestoreBindingState(const std::string& id, const std::string& snapshot)
    {
        const auto& slots = GetBindingSlots(id);
        if (!m_adapter || slots.empty()) return false;
        Input::InputProfile state;
        std::vector<std::string> diagnostics;
        if (!NLS::Core::Inputs::ParseInputProfile(snapshot, state, diagnostics)) return false;
        const auto valid = [&](const auto& item) { return std::find(slots.begin(), slots.end(), item.binding) != slots.end(); };
        if (state.overrides.size() != slots.size() || !std::all_of(state.overrides.begin(), state.overrides.end(), valid) ||
            !std::all_of(state.unresolved.begin(), state.unresolved.end(), valid)) return false;
        auto profile = m_adapter->GetUser().GetProfile();
        for (auto binding : slots)
        {
            std::erase_if(profile.overrides, [&](const auto& item) { return item.binding == binding; });
            std::erase_if(profile.unresolved, [&](const auto& item) { return item.binding == binding; });
        }
        profile.overrides.insert(profile.overrides.end(), state.overrides.begin(), state.overrides.end());
        profile.unresolved.insert(profile.unresolved.end(), state.unresolved.begin(), state.unresolved.end());
        m_adapter->GetUser().SetProfile(std::move(profile));
        return true;
    }

    const EditorShortcutRegistry& EditorShortcutService::GetRegistry() const
    {
        return m_registry;
    }

    EditorShortcutRegistry& EditorShortcutService::GetRegistry()
    {
        return m_registry;
    }

    bool EditorShortcutService::AreInputContextsMutuallyExclusive(Input::ContextId first, Input::ContextId second) const
    {
        if (!m_adapter) return false;
        std::string a, b;
        if (first == EditorShortcutAdapter::ContextForScope(ShortcutContexts::Global)) a = ShortcutContexts::Global;
        if (second == EditorShortcutAdapter::ContextForScope(ShortcutContexts::Global)) b = ShortcutContexts::Global;
        for (const auto& context : m_contexts)
        {
            const auto id = EditorShortcutAdapter::ContextForScope(context.id);
            if (id == first) a = context.id;
            if (id == second) b = context.id;
        }
        // Owned documents disable the ordinary Global context in the router.
        // Their Save/Undo defaults can therefore share bindings with Global.
        const auto ownsGlobal = [](const std::string& scope)
        {
            return scope == "ui-designer" || scope == "vfx-editor" || scope == "vfx-library" ||
                scope == "animation-workbench" || scope == "input-editor" || scope == "node-graph" || scope == "node-graph-clipboard";
        };
        if ((a == ShortcutContexts::Global && ownsGlobal(b)) || (b == ShortcutContexts::Global && ownsGlobal(a))) return true;
        return AreContextsMutuallyExclusive(a, b);
    }

    bool EditorShortcutService::AreContextsMutuallyExclusive(
        const ShortcutContextId& p_first,
        const ShortcutContextId& p_second) const
    {
        if (p_first == p_second)
            return false;
        if ((p_first == ShortcutContexts::SceneViewFlyMode && p_second == ShortcutContexts::SceneView) ||
            (p_second == ShortcutContexts::SceneViewFlyMode && p_first == ShortcutContexts::SceneView)) return true;
        if (p_first == ShortcutContexts::Global || p_second == ShortcutContexts::Global)
            return false;

        const ShortcutContext* first = nullptr;
        const ShortcutContext* second = nullptr;
        for (const auto& context : m_contexts)
        {
            if (context.id == p_first)
                first = &context;
            if (context.id == p_second)
                second = &context;
        }

        return first != nullptr &&
            second != nullptr &&
            !first->exclusiveGroup.empty() &&
            first->exclusiveGroup == second->exclusiveGroup;
    }
}
