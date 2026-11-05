#pragma once

#include "InputTypes.h"
#include <functional>
#include <span>
#include <vector>

namespace NLS::Input
{
// One instance per host. Platform callbacks submit records; consumers never poll
// an OS backend through this class. Handles remain checked across slot reuse.
class NLS_INPUT_API InputSystem final
{
public:
    using Clock = std::function<double()>;
    explicit InputSystem(Clock clock = {});
    ~InputSystem();
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    DeviceId GetKeyboard() const { return m_keyboard; }
    DeviceId GetMouse() const { return m_mouse; }
    DeviceId RegisterDevice(DeviceDescriptor descriptor);
    bool RemoveDevice(DeviceId device);
    const DeviceState* FindDevice(DeviceId device) const;
    std::span<const DeviceState> GetDevices() const { return m_devices; }

    WindowId RegisterWindow(uintptr_t nativeHandle, IngressKind ingress, bool focused = false);
    bool UnregisterWindow(WindowId window);
    const WindowState* GetWindowState(WindowId window) const;
    std::span<const WindowState> GetWindows() const { return m_windows; }

    bool SubmitEvent(InputEvent event);
    std::span<const InputEvent> GetEvents() const { return m_events; }
    uint64_t AllocateSequence() { return ++m_sequence; }
    uint64_t GetLastSequence() const { return m_sequence; }
    uint64_t GetFrameIndex() const { return m_frameIndex; }
    uint64_t AllocateUserId() { return ++m_userSequence; }
    uint64_t GetRejectedEventCount() const { return m_rejectedEvents; }
    double Now() const;
    void EndFrame();
    // Seal the platform pump before running consumers. Events submitted by a
    // command's nested native message loop survive EndFrame for the next update.
    void FinishEventPump();

    bool IsButtonDown(DeviceId device, uint16_t control) const;
    bool WasButtonPressed(DeviceId device, uint16_t control) const;
    bool WasButtonReleased(DeviceId device, uint16_t control) const;
    Maths::Vector2 GetMousePosition() const;
    Maths::Vector2 GetPointerScreenPosition() const;
    WindowId GetPointerWindow() const { return m_pointerWindow; }
    Maths::Vector2 GetMouseDelta() const;
    Maths::Vector2 GetMouseWheel() const;
    GamepadState GetGamepadState(DeviceId device) const;
    bool HasTransientEvents() const;

private:
    DeviceState* FindMutableDevice(DeviceId device);
    WindowState* FindMutableWindow(WindowId window);
    void ResetDevice(DeviceState& state);
    Clock m_clock;
    std::vector<DeviceState> m_devices;
    std::vector<WindowState> m_windows;
    std::vector<InputEvent> m_events;
    struct DeferredTransients
    {
        std::array<bool, 512> pressed{};
        std::array<bool, 512> released{};
        Maths::Vector2 delta{};
        Maths::Vector2 wheel{};
    };
    DeviceId m_keyboard;
    DeviceId m_mouse;
    uint64_t m_sequence = 0;
    uint64_t m_frameIndex = 0;
    uint64_t m_rejectedEvents = 0;
    uint64_t m_userSequence = 0;
    WindowId m_pointerWindow;
    double m_lastTimestamp = 0.0;
    std::vector<DeferredTransients> m_deferredTransients;
    uint64_t m_pumpSequence = 0;
    bool m_pumpFinished = false;
};
}
