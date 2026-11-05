#include "InputReplay.h"
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace NLS::Input
{
bool ReplayInput(std::span<const InputReplayRecord> records,
    std::vector<InputReplayBatch>& result, std::vector<std::string>& diagnostics)
{
    double time = 0;
    InputSystem system([&] { return time; });
    std::unique_ptr<InputDomain> domain;
    std::unordered_map<uint64_t, DeviceId> devices{
        {system.GetKeyboard().value, system.GetKeyboard()}, {system.GetMouse().value, system.GetMouse()}};
    std::unordered_map<uint64_t, WindowId> windows;
    std::unordered_map<uint64_t, uint64_t> deviceKeys{
        {system.GetKeyboard().value, system.GetKeyboard().value}, {system.GetMouse().value, system.GetMouse().value}};
    std::unordered_map<uint64_t, uint64_t> windowKeys;
    std::unordered_map<uint64_t, std::shared_ptr<InputUser>> users;
    std::unordered_map<uint64_t, uint64_t> sequences;
    std::unordered_map<uint64_t, uint64_t> collected;
    std::vector<InputReplayBatch> output;
    const auto device = [&](uint64_t key) -> DeviceId
    {
        const auto it = devices.find(key);
        if (it == devices.end()) throw std::invalid_argument("Replay references an unknown device");
        return it->second;
    };
    const auto user = [&](uint64_t key) -> InputUser&
    {
        const auto it = users.find(key);
        if (it == users.end()) throw std::invalid_argument("Replay references an unknown user");
        return *it->second;
    };
    const auto remapPath = [&](ControlPath& path)
    {
        if (auto it = devices.find(path.deviceId.value); path.deviceId && it != devices.end()) path.deviceId = it->second;
    };
    const auto remapChord = [&](Chord& chord) { for (auto& path : chord.controls) remapPath(path); };
    const auto collect = [&](UpdateMode mode)
    {
        // Preserve user creation order rather than unordered-map iteration order.
        for (const auto& current : domain->GetUsers())
        {
            InputReplayBatch batch; batch.inputFrame = system.GetFrameIndex(); batch.mode = mode;
            for (const auto& [key, value] : users) if (value == current) { batch.user = key; break; }
            if (mode == UpdateMode::Fixed) batch.frame = current->GetFixedFrame();
            else
            {
                batch.frame.epoch = current->GetEpoch();
                for (const auto& event : current->GetActionEvents())
                    if (event.sequence > collected[batch.user]) batch.frame.events.push_back(event);
                if (!batch.frame.events.empty()) collected[batch.user] = batch.frame.events.back().sequence;
                if (auto definition = current->GetDefinition())
                    for (const auto& action : definition->GetDefinition().actions)
                        batch.frame.actions.push_back(current->GetActionState(current->FindAction(action.id)));
            }
            for (auto& event : batch.frame.events)
            {
                if (auto it = sequences.find(event.sourceSequence); it != sequences.end()) event.sourceSequence = it->second;
                if (auto it = deviceKeys.find(event.device.value); it != deviceKeys.end()) event.device = {it->second};
                if (auto it = windowKeys.find(event.window.value); it != windowKeys.end()) event.window = {it->second};
            }
            output.push_back(std::move(batch));
        }
    };
    size_t index = 0;
    try
    {
        for (const auto& record : records)
        {
            ++index;
            if (record.operation > ReplayOperation::FinishEventPump) throw std::invalid_argument("Unknown replay operation");
            switch (record.operation)
            {
            case ReplayOperation::BeginDomain:
                if (domain) throw std::invalid_argument("Replay domain already exists");
                domain = std::make_unique<InputDomain>(system, "Replay"); break;
            case ReplayOperation::StopDomain:
                if (!domain) throw std::invalid_argument("Replay domain does not exist");
                domain->Cancel(CancelReason::Stop); domain->DispatchDynamicEvents(); collect(UpdateMode::Dynamic);
                users.clear(); collected.clear(); domain.reset(); break;
            case ReplayOperation::AddDevice:
                if (!record.object || devices.contains(record.object)) throw std::invalid_argument("Duplicate replay device");
                if (record.device.kind != DeviceKind::Gamepad) throw std::invalid_argument("Replay has one shared keyboard and mouse");
                devices.emplace(record.object, system.RegisterDevice(record.device));
                deviceKeys[devices.at(record.object).value] = record.object; break;
            case ReplayOperation::RemoveDevice:
                if (!system.RemoveDevice(device(record.object))) throw std::invalid_argument("Cannot remove replay device");
                devices.erase(record.object); break;
            case ReplayOperation::AddWindow:
                if (!record.object || windows.contains(record.object)) throw std::invalid_argument("Duplicate replay window");
                windows.emplace(record.object, system.RegisterWindow(static_cast<uintptr_t>(record.object), IngressKind::Virtual, record.enabled));
                windowKeys[windows.at(record.object).value] = record.object; break;
            case ReplayOperation::RemoveWindow:
                if (!windows.contains(record.object) || !system.UnregisterWindow(windows.at(record.object))) throw std::invalid_argument("Cannot remove replay window");
                windows.erase(record.object); break;
            case ReplayOperation::Event:
            {
                auto event = record.event;
                if (event.device) event.device = device(event.device.value);
                if (event.window)
                {
                    if (!windows.contains(event.window.value)) throw std::invalid_argument("Replay references an unknown window");
                    event.window = windows.at(event.window.value);
                }
                event.ingress = IngressKind::Virtual;
                if (!system.SubmitEvent(event)) throw std::invalid_argument("Invalid replay device event");
                if (record.event.sequence) sequences[system.GetLastSequence()] = record.event.sequence;
                break;
            }
            case ReplayOperation::Definition:
            {
                if (!domain || !record.user) throw std::invalid_argument("Definition requires a replay user/domain");
                if (!users.contains(record.user)) users.emplace(record.user, domain->CreateUser());
                auto compiled = record.definition;
                if (compiled)
                {
                    auto data = compiled->GetDefinition();
                    for (auto& action : data.actions) remapChord(action.chord);
                    for (auto& context : data.contexts) for (auto& binding : context.bindings)
                    {
                        for (auto& part : binding.parts) remapPath(part.path);
                        remapChord(binding.chord);
                    }
                    compiled = CompiledInput::Compile(std::move(data), diagnostics);
                    if (!compiled) throw std::invalid_argument("Invalid remapped replay definition");
                }
                user(record.user).SetDefinition(std::move(compiled)); break;
            }
            case ReplayOperation::Profile:
            {
                auto profile = record.profile;
                for (auto* entries : {&profile.overrides, &profile.unresolved})
                    for (auto& entry : *entries)
                    {
                        for (auto& part : entry.parts) remapPath(part.path);
                        if (entry.chord) remapChord(*entry.chord);
                    }
                user(record.user).SetProfile(std::move(profile)); break;
            }
            case ReplayOperation::PushContext:
                if (!user(record.user).PushContext(record.context, record.priority)) throw std::invalid_argument("Unknown replay context"); break;
            case ReplayOperation::PopContext:
                if (!user(record.user).PopContext(record.context)) throw std::invalid_argument("Inactive replay context"); break;
            case ReplayOperation::EnableContext:
                if (!user(record.user).EnableContext(record.context)) throw std::invalid_argument("Unknown replay context"); break;
            case ReplayOperation::DisableContext:
                if (!user(record.user).DisableContext(record.context)) throw std::invalid_argument("Unknown replay context"); break;
            case ReplayOperation::PairDevice: user(record.user).PairDevice(device(record.object)); break;
            case ReplayOperation::UnpairDevice: user(record.user).UnpairDevice(device(record.object)); break;
            case ReplayOperation::BlockDevice: user(record.user).SetDeviceBlocked(record.deviceKind, record.enabled); break;
            case ReplayOperation::SuspendUser: user(record.user).SetSuspended(record.enabled, record.reason); break;
            case ReplayOperation::EnableUser: user(record.user).SetEnabled(record.enabled); break;
            case ReplayOperation::CancelUser: user(record.user).Cancel(record.reason, record.reacquire); break;
            case ReplayOperation::SuspendDomain:
                if (!domain) throw std::invalid_argument("Replay domain does not exist");
                domain->SetSuspended(record.enabled, record.reason); break;
            case ReplayOperation::AdvanceTime:
            case ReplayOperation::Update:
                if (!std::isfinite(record.time) || record.time < system.Now()) throw std::invalid_argument("Invalid replay time");
                time = record.time;
                if (record.operation == ReplayOperation::Update)
                {
                    if (!domain) throw std::invalid_argument("Replay domain does not exist");
                    domain->Update(time); collect(UpdateMode::Dynamic);
                }
                break;
            case ReplayOperation::FixedTick:
                if (!domain) throw std::invalid_argument("Replay domain does not exist");
                domain->BeginFixedTick(record.object, record.time); collect(UpdateMode::Fixed); break;
            case ReplayOperation::EndFrame: system.EndFrame(); break;
            case ReplayOperation::FinishEventPump: system.FinishEventPump(); break;
            }
        }
    }
    catch (const std::exception& error)
    {
        diagnostics.push_back("Replay record " + std::to_string(index) + ": " + error.what()); return false;
    }
    result = std::move(output); return true;
}
}
