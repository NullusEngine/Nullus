#include "InputSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace NLS::Input
{
namespace
{
size_t Slot(uint64_t value) { return static_cast<uint32_t>(value) - size_t{1}; }
uint64_t Handle(size_t slot, uint64_t previous)
{
    const auto generation = static_cast<uint32_t>((previous >> 32u) + 1u);
    return (uint64_t{generation == 0 ? 1u : generation} << 32u) | uint64_t{slot + 1u};
}
}

InputSystem::InputSystem(Clock clock) : m_clock(std::move(clock))
{
    if (!m_clock)
    {
        const auto origin = std::chrono::steady_clock::now();
        m_clock = [origin] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count(); };
    }
    m_devices.reserve(18);
    m_deferredTransients.reserve(18);
    m_windows.reserve(8);
    m_events.reserve(1024);
    m_keyboard = RegisterDevice({DeviceKind::Keyboard, "Keyboard", "Keyboard", {}, false});
    m_mouse = RegisterDevice({DeviceKind::Mouse, "Mouse", "Mouse", {}, false});
}
InputSystem::~InputSystem() = default;

double InputSystem::Now() const { return std::max(m_lastTimestamp, m_clock()); }

DeviceId InputSystem::RegisterDevice(DeviceDescriptor descriptor)
{
    size_t slot = 0;
    while (slot < m_devices.size() && m_devices[slot].connected) ++slot;
    if (slot == m_devices.size())
    {
        m_devices.emplace_back();
        m_deferredTransients.emplace_back();
    }
    const DeviceId id{Handle(slot, m_devices[slot].id.value)};
    m_devices[slot] = {};
    m_deferredTransients[slot] = {};
    auto& state = m_devices[slot];
    state.id = id;
    state.descriptor = std::move(descriptor);
    state.connected = true;
    InputEvent event;
    event.device = id;
    event.type = InputEventType::DeviceAdded;
    SubmitEvent(event);
    return id;
}

DeviceState* InputSystem::FindMutableDevice(DeviceId device)
{
    if (!device || Slot(device.value) >= m_devices.size()) return nullptr;
    auto& state = m_devices[Slot(device.value)];
    return state.id == device ? &state : nullptr;
}
const DeviceState* InputSystem::FindDevice(DeviceId device) const
{
    return const_cast<InputSystem*>(this)->FindMutableDevice(device);
}
void InputSystem::ResetDevice(DeviceState& state)
{
    state.buttons.fill(false);
    state.axes.fill(0.0f);
    state.delta = {};
    state.wheel = {};
    state.hasPointerPosition = false;
    state.hasScreenPosition = false;
    state.synchronized = false;
    auto& deferred = m_deferredTransients[Slot(state.id.value)];
    deferred.delta = {}; deferred.wheel = {};
}
bool InputSystem::RemoveDevice(DeviceId device)
{
    if (device == m_keyboard || device == m_mouse) return false;
    auto* state = FindMutableDevice(device);
    if (!state || !state->connected) return false;
    InputEvent event;
    event.device = device;
    event.type = InputEventType::DeviceRemoved;
    return SubmitEvent(event);
}

WindowId InputSystem::RegisterWindow(uintptr_t nativeHandle, IngressKind ingress, bool focused)
{
    for (const auto& state : m_windows)
        if (state.registered && state.nativeHandle == nativeHandle)
            return state.ingress == ingress ? state.id : WindowId{};
    size_t slot = 0;
    while (slot < m_windows.size() && m_windows[slot].registered) ++slot;
    if (slot == m_windows.size()) m_windows.emplace_back();
    auto& state = m_windows[slot];
    const WindowId id{Handle(slot, state.id.value)};
    state = {id, nativeHandle, ingress, true, focused};
    return id;
}
WindowState* InputSystem::FindMutableWindow(WindowId window)
{
    if (!window || Slot(window.value) >= m_windows.size()) return nullptr;
    auto& state = m_windows[Slot(window.value)];
    return state.registered && state.id == window ? &state : nullptr;
}
const WindowState* InputSystem::GetWindowState(WindowId window) const
{
    return const_cast<InputSystem*>(this)->FindMutableWindow(window);
}
bool InputSystem::UnregisterWindow(WindowId window)
{
    auto* state = FindMutableWindow(window);
    if (!state) return false;
    InputEvent event;
    event.window = window;
    event.ingress = state->ingress;
    event.type = InputEventType::FocusLost;
    SubmitEvent(event);
    state->registered = false;
    return true;
}

bool InputSystem::SubmitEvent(InputEvent event)
{
    auto reject = [this] { ++m_rejectedEvents; return false; };
    if (static_cast<uint8_t>(event.type) > static_cast<uint8_t>(InputEventType::DeviceReset) ||
        static_cast<uint8_t>(event.ingress) > static_cast<uint8_t>(IngressKind::Win32)) return reject();
    WindowState* window = nullptr;
    if (event.window)
    {
        window = FindMutableWindow(event.window);
        if (!window || window->ingress != event.ingress) return reject();
    }
    if (event.timestamp < 0.0) event.timestamp = Now();
    if (!std::isfinite(event.timestamp) || event.timestamp < m_lastTimestamp ||
        !std::isfinite(event.x) || !std::isfinite(event.y) ||
        !std::isfinite(event.screenX) || !std::isfinite(event.screenY)) return reject();
    auto* device = event.device ? FindMutableDevice(event.device) : nullptr;
    if (event.device && (!device || (!device->connected && event.type != InputEventType::DeviceAdded))) return reject();
    auto* deferred = device && m_pumpFinished ? &m_deferredTransients[Slot(event.device.value)] : nullptr;
    switch (event.type)
    {
    case InputEventType::ButtonDown:
    case InputEventType::ButtonUp:
    case InputEventType::ButtonRepeat:
        if (!device || event.control >= device->buttons.size()) return reject();
        if (device->descriptor.kind == DeviceKind::Mouse && event.type == InputEventType::ButtonDown && event.window) m_pointerWindow = event.window;
        if ((static_cast<unsigned int>(event.flags) & static_cast<unsigned int>(InputEventFlags::FocusReset)) != 0)
            break; // GLFW focus-release notifications are not physical key-ups.
        if (event.type == InputEventType::ButtonDown)
        {
            if (!device->buttons[event.control])
            {
                if (deferred) deferred->pressed[event.control] = true;
                else device->pressed[event.control] = true;
            }
            device->buttons[event.control] = true;
        }
        else if (event.type == InputEventType::ButtonUp)
        {
            if (deferred) deferred->released[event.control] = true;
            else device->released[event.control] = true;
            device->buttons[event.control] = false;
        }
        device->synchronized = true;
        break;
    case InputEventType::Axis:
        if (!device || event.control >= device->axes.size()) return reject();
        device->axes[event.control] = static_cast<float>(event.x);
        break;
    case InputEventType::PointerMove:
        if (!device) return reject();
        if (device->hasPointerPosition && (static_cast<unsigned int>(event.flags) & 3u) == 0)
        {
            const bool screen = (static_cast<unsigned int>(event.flags) & 4u) != 0;
            const auto previous = screen ? device->screenPosition : device->position;
            const Maths::Vector2 delta{static_cast<float>(screen ? event.screenX : event.x) - previous.x,
                static_cast<float>(screen ? event.screenY : event.y) - previous.y};
            if (deferred) deferred->delta += delta;
            else device->delta += delta;
        }
        device->position = {static_cast<float>(event.x), static_cast<float>(event.y)};
        device->screenPosition = {static_cast<float>(event.screenX), static_cast<float>(event.screenY)};
        device->hasPointerPosition = true;
        device->hasScreenPosition = (static_cast<unsigned int>(event.flags) & 4u) != 0;
        m_pointerWindow = event.window;
        break;
    case InputEventType::PointerDelta:
        if (!device) return reject();
        if (deferred) deferred->delta += Maths::Vector2{static_cast<float>(event.x), static_cast<float>(event.y)};
        else device->delta += Maths::Vector2{static_cast<float>(event.x), static_cast<float>(event.y)};
        break;
    case InputEventType::Wheel:
        if (!device) return reject();
        if (deferred) deferred->wheel += Maths::Vector2{static_cast<float>(event.x), static_cast<float>(event.y)};
        else device->wheel += Maths::Vector2{static_cast<float>(event.x), static_cast<float>(event.y)};
        break;
    case InputEventType::DeviceRemoved:
        if (!device || event.device == m_keyboard || event.device == m_mouse) return reject();
        ResetDevice(*device);
        device->connected = false;
        break;
    case InputEventType::DeviceReset:
        if (!device) return reject();
        ResetDevice(*device);
        break;
    case InputEventType::FocusGained:
    case InputEventType::FocusLost:
        if (!window) return reject();
        window->focused = event.type == InputEventType::FocusGained;
        break;
    default: break;
    }
    event.sequence = AllocateSequence();
    m_lastTimestamp = event.timestamp;
    m_events.push_back(event);
    return true;
}

bool InputSystem::IsButtonDown(DeviceId id, uint16_t control) const
{
    const auto* state = FindDevice(id);
    return state && state->connected && state->synchronized && control < state->buttons.size() && state->buttons[control];
}
bool InputSystem::WasButtonPressed(DeviceId id, uint16_t control) const
{
    const auto* state = FindDevice(id);
    return state && control < state->pressed.size() && state->pressed[control];
}
bool InputSystem::WasButtonReleased(DeviceId id, uint16_t control) const
{
    const auto* state = FindDevice(id);
    return state && control < state->released.size() && state->released[control];
}
Maths::Vector2 InputSystem::GetMousePosition() const { return FindDevice(m_mouse)->position; }
Maths::Vector2 InputSystem::GetPointerScreenPosition() const { return FindDevice(m_mouse)->screenPosition; }
Maths::Vector2 InputSystem::GetMouseDelta() const { return FindDevice(m_mouse)->delta; }
Maths::Vector2 InputSystem::GetMouseWheel() const { return FindDevice(m_mouse)->wheel; }
GamepadState InputSystem::GetGamepadState(DeviceId id) const
{
    GamepadState result;
    const auto* state = FindDevice(id);
    if (!state || !state->connected || state->descriptor.kind != DeviceKind::Gamepad) return result;
    result.id = id;
    result.connected = true;
    result.mapped = state->descriptor.layout == "Gamepad";
    std::copy_n(state->buttons.begin(), result.buttons.size(), result.buttons.begin());
    std::copy_n(state->axes.begin(), result.axes.size(), result.axes.begin());
    return result;
}
bool InputSystem::HasTransientEvents() const
{
    return std::any_of(m_events.begin(), m_events.end(), [](const InputEvent& event)
    {
        return event.type != InputEventType::DeviceAdded &&
            !(event.type == InputEventType::PointerMove &&
              (static_cast<uint8_t>(event.flags) & (static_cast<uint8_t>(InputEventFlags::PointerWarp) | static_cast<uint8_t>(InputEventFlags::Resynchronize))));
    });
}
void InputSystem::EndFrame()
{
    ++m_frameIndex;
    for (size_t i = 0; i < m_devices.size(); ++i)
    {
        auto& device = m_devices[i];
        const auto& deferred = m_deferredTransients[i];
        device.pressed = deferred.pressed;
        device.released = deferred.released;
        device.delta = deferred.delta;
        device.wheel = deferred.wheel;
        m_deferredTransients[i] = {};
    }
    if (m_pumpFinished)
        m_events.erase(m_events.begin(), std::upper_bound(m_events.begin(), m_events.end(), m_pumpSequence,
            [](uint64_t sequence, const InputEvent& event) { return sequence < event.sequence; }));
    else m_events.clear();
    m_pumpFinished = false;
}
void InputSystem::FinishEventPump()
{
    if (std::any_of(m_windows.begin(), m_windows.end(), [](const WindowState& state) { return state.registered; }) &&
        std::none_of(m_windows.begin(), m_windows.end(), [](const WindowState& state) { return state.registered && state.focused; }))
    {
        for (const auto id : {m_keyboard, m_mouse})
        {
            if (!FindDevice(id)->synchronized) continue;
            InputEvent event;
            event.type = InputEventType::DeviceReset;
            event.flags = InputEventFlags::FocusReset;
            event.device = id;
            SubmitEvent(event);
        }
    }
    m_pumpSequence = m_sequence;
    m_pumpFinished = true;
    for (auto& deferred : m_deferredTransients) deferred = {};
}
}
