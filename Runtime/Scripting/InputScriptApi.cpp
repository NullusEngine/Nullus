#include "InputScriptApi.h"
#include "ScriptEngineApi.h"
#include <Input/InputDomain.h>
#include <Input/InputRebinding.h>
#include <Core/Inputs/InputSerialization.h>
#include <Core/ServiceLocator.h>
#include <Windowing/Inputs/InputDisplay.h>
#include <unordered_map>
#include <algorithm>
#include <Engine/Components/PlayerInputComponent.h>

namespace NLS::Scripting
{
namespace
{
std::shared_ptr<Input::InputUser> User(uint64_t id)
{
    if (!ScriptEngineApi::IsAcceptingPlayRequests() || !Core::ServiceLocator::Contains<Input::InputDomain>()) return {};
    for (const auto& user : Core::ServiceLocator::Get<Input::InputDomain>().GetUsers()) if (!id || user->GetId() == id) return user;
    return {};
}
struct Rebind
{
    // Session borrows User. Destruction order keeps User alive until resumed.
    std::shared_ptr<Input::InputUser> user;
    std::unique_ptr<Input::RebindingSession> session;
    double expires = 0;
    Input::RebindingStatus finalStatus = Input::RebindingStatus::Canceled;
};
std::unordered_map<uint64_t, Rebind> rebindings;
uint64_t nextRebind = 0;
std::optional<Guid> ResolveAction(const Input::InputUser& user, std::string_view text, bool events = false)
{
    const auto definition = user.GetDefinition();
    if (!definition) return {};
    // Persistent references qualify the ActionId with the immutable definition ID.
    if (text.size() == 73 && text[36] == '/')
    {
        const auto asset = Guid::TryParse(text.substr(0, 36));
        const auto action = Guid::TryParse(text.substr(37));
        if (asset && action)
            return events || (*asset == definition->GetDefinition().id && definition->FindAction(*action) != SIZE_MAX) ? action : std::nullopt;
    }
    if (events) if (const auto action = Guid::TryParse(text)) return action;
    const auto index = definition->FindAction(text);
    return index == SIZE_MAX ? std::nullopt : std::optional<Guid>(definition->GetDefinition().actions[index].id);
}
}
bool InputScriptApi::Read(uint64_t id, const char* text, uint32_t operation, uint64_t after, ScriptInputValue& result)
{
    result = {};
    auto user = User(id);
    if (!user || !text || operation > 3) return false;
    result.epoch = user->GetEpoch();
    const auto guid = ResolveAction(*user, text, operation >= 2);
    if (!guid) return true; // removed/unassigned/wrong-asset reference is unavailable
    if (operation >= 2)
    {
        const std::span<const Input::ActionEvent> events = operation == 3 ? std::span<const Input::ActionEvent>(user->GetFixedFrame().events) : user->GetActionEvents();
        const std::string_view selector(text);
        const bool differentAsset = selector.size() == 73 && selector[36] == '/' &&
            Guid::TryParse(selector.substr(0, 36)) != std::optional<Guid>(user->GetDefinition()->GetDefinition().id);
        for (const auto& event : events)
            if (event.action == *guid && event.sequence > after && event.epoch == user->GetEpoch() &&
                (!differentAsset || (event.phase == Input::ActionPhase::Canceled && event.reason == Input::CancelReason::DefinitionChanged)))
            {
                result.x = event.value.vector.x; result.y = event.value.vector.y; result.z = event.value.vector.z;
                result.flags = 32; result.sequence = event.sequence; result.tick = event.tick; result.epoch = event.epoch;
                result.timestamp = event.realTimestamp; result.phase = static_cast<uint32_t>(event.phase); result.reason = static_cast<uint32_t>(event.reason); return true;
            }
        return true;
    }
    const auto handle = user->FindAction(*guid);
    if (!user->GetDefinition() || user->GetDefinition()->FindAction(*guid) == SIZE_MAX) return true;
    if (operation < 2)
    {
        auto snapshot = user->GetActionState(handle);
        if (operation == 1)
        {
            const auto& frame = user->GetFixedFrame();
            if (handle.index >= frame.actions.size() || frame.epoch != user->GetEpoch()) return true;
            snapshot = frame.actions[handle.index]; result.tick = frame.tick;
        }
        result.x = snapshot.value.vector.x; result.y = snapshot.value.vector.y; result.z = snapshot.value.vector.z;
        result.flags = 64u | (snapshot.actuated ? 1u : 0u) | (snapshot.pressed ? 2u : 0u) | (snapshot.released ? 4u : 0u) | (snapshot.performed ? 8u : 0u) | (snapshot.canceled ? 16u : 0u);
        return true;
    }
    return true;
}
bool InputScriptApi::Command(uint64_t id, uint32_t operation, const char* text, uint64_t argument, uint64_t& result, std::string& error)
{
    result = 0;
    if (operation == 22)
    {
        if (!ScriptEngineApi::IsAcceptingPlayRequests()) { error = "Play session is no longer active"; return false; }
        const auto* component = dynamic_cast<Engine::Components::PlayerInputComponent*>(Object::IDToPointerNoThreadCheck(NativeObjectHandle{argument}.GetInstanceId()));
        const auto user = component ? component->GetInputUser() : nullptr;
        if (!user || !Core::ServiceLocator::Contains<Input::InputDomain>()) { error = "Player Input has no active input user"; return false; }
        const auto users = Core::ServiceLocator::Get<Input::InputDomain>().GetUsers();
        if (std::find(users.begin(), users.end(), user) == users.end()) { error = "Player Input belongs to an expired input domain"; return false; }
        result = user->GetId(); return true;
    }
    if (operation >= 14 && operation <= 18)
    {
        if (!ScriptEngineApi::IsAcceptingPlayRequests() || !Core::ServiceLocator::Contains<Input::InputDomain>()) { error = "Input domain is no longer active"; return false; }
        auto& domain = Core::ServiceLocator::Get<Input::InputDomain>();
        if (operation == 14)
        { const auto users = domain.GetUsers(); result = argument < users.size() ? users[argument]->GetId() : 0; return true; }
        auto& system = domain.GetInputSystem();
        if (operation == 15)
        {
            uint64_t index = 0;
            for (const auto& device : system.GetDevices()) if (device.connected && index++ == argument) { result = device.id.value; break; }
        }
        else
        {
            const auto* device = system.FindDevice({argument});
            if (operation == 16) result = device ? static_cast<uint64_t>(device->descriptor.kind) : 3u;
            else if (operation == 17) result = device && device->connected;
            else result = device && device->connected && (device->descriptor.kind != Input::DeviceKind::Gamepad || device->descriptor.layout == "Gamepad");
        }
        return true;
    }
    auto user = User(id); if (!user) { error = "Input user is no longer active"; return false; }
    if (operation == 20 || operation == 21)
    {
        const auto action = ResolveAction(*user, text ? text : "");
        if (!action) return true;
        // Two ABI integers carry canonical UUID bytes; independent of host endian/layout.
        for (size_t i = operation == 20 ? 0 : 8, end = i + 8; i < end; ++i) result = (result << 8) | action->GetBytes()[i];
        return true;
    }
    if ((operation >= 9 && operation <= 11) || operation == 19)
    {
        auto it = rebindings.find(argument);
        if (operation == 11 && it == rebindings.end()) return true; // idempotent disposal
        if (it == rebindings.end() || it->second.user != user) { error = "Rebinding session expired"; return false; }
        if (operation == 9) { result = static_cast<uint64_t>(it->second.session ? it->second.session->Update() : it->second.finalStatus); return true; }
        if (operation == 19)
        {
            if (!it->second.session) return true; // finished operations have no active candidate
            const auto& binding = it->second.session->GetResult();
            result = Input::FindBindingConflicts(*user, binding.binding, binding.parts, binding.chord.value_or(Input::Chord{})).size(); return true;
        }
        const bool applied = operation == 11 || (it->second.session && it->second.session->Apply());
        if (!applied) error = "Rebinding did not complete";
        rebindings.erase(it); return applied;
    }
    if (operation == 12) return Core::Inputs::SaveInputProfile(text ? text : "", user->GetProfile(), error);
    if (operation == 13)
    {
        Input::InputProfile profile; std::vector<std::string> diagnostics;
        if (!Core::Inputs::LoadInputProfile(text ? text : "", profile, diagnostics)) { error = diagnostics.empty() ? "Profile load failed" : diagnostics.front(); return false; }
        user->SetProfile(std::move(profile)); return true;
    }
    if (operation == 6 || operation == 7)
    {
        if (operation == 6)
        {
            const auto* device = Core::ServiceLocator::Get<Input::InputDomain>().GetInputSystem().FindDevice({argument});
            if (!device || !device->connected) { error = "Input device is disconnected or stale"; return false; }
            user->PairDevice({argument});
        }
        else user->UnpairDevice({argument});
        return true;
    }
    auto guid = text ? Guid::TryParse(text) : std::nullopt;
    if (!guid) { error = "A stable input id is required"; return false; }
    switch (operation)
    {
    case 0: return user->PushContext(*guid, static_cast<int>(argument));
    case 1: return user->PopContext(*guid);
    case 2: return user->EnableContext(*guid);
    case 3: return user->DisableContext(*guid);
    case 4: user->RestoreBindingDefault(*guid); return true;
    case 5: user->ClearBinding(*guid); return true;
    case 8:
        if (!user->GetDefinition() || !user->GetDefinition()->HasBinding(*guid)) return false;
        std::erase_if(rebindings, [&](const auto& item) { return item.second.user == user && !item.second.session; });
        for (const auto& [token, entry] : rebindings) if (entry.user == user) { error = "This input user is already rebinding"; return false; }
        result = ++nextRebind;
        {
            auto& system = Core::ServiceLocator::Get<Input::InputDomain>().GetInputSystem();
            rebindings.emplace(result, Rebind{user, std::make_unique<Input::RebindingSession>(system, *user, *guid), system.Now() + 10});
        }
        return true;
    default: error = "Unknown input command"; return false;
    }
}
std::string InputScriptApi::BindingDisplayName(uint64_t userId, const char* text)
{
    auto user = User(userId); auto guid = text ? Guid::TryParse(text) : std::nullopt;
    return user && guid ? Windowing::Inputs::GetBindingDisplayName(*user, *guid) : std::string{};
}
void InputScriptApi::Reset() { rebindings.clear(); }
void InputScriptApi::UpdateRebindings()
{
    if (rebindings.empty() || !Core::ServiceLocator::Contains<Input::InputDomain>()) return;
    const auto& domain = Core::ServiceLocator::Get<Input::InputDomain>();
    const auto now = domain.GetInputSystem().Now();
    const auto users = domain.GetUsers();
    for (auto it = rebindings.begin(); it != rebindings.end();)
    {
        auto& entry = it->second;
        if (std::find(users.begin(), users.end(), entry.user) == users.end()) { it = rebindings.erase(it); continue; }
        if (entry.session)
        {
            const auto status = entry.session->Update();
            if (now >= entry.expires || status == Input::RebindingStatus::Canceled || status == Input::RebindingStatus::TimedOut)
            {
                entry.finalStatus = now >= entry.expires ? Input::RebindingStatus::TimedOut : status;
                entry.session.reset(); // release the lease while retaining one terminal status per user
            }
        }
        ++it;
    }
}
}
