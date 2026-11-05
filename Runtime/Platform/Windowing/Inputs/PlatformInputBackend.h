#pragma once
#include "PlatformDef.h"
#include "WindowInputEvent.h"
#include "TextInputService.h"
#include <Eventing/Event.h>
#include <Input/InputSystem.h>
#include <array>
#include <memory>
#include <unordered_map>

struct GLFWwindow;
namespace NLS::Windowing { class Window; }
namespace NLS::Windowing::Inputs
{
// Supplied by the UI adapter; Platform does not include or link ImGui.
struct InputForwardingContext
{
    void* context = nullptr;
    void* (*activate)(void*) = nullptr;
    void (*restore)(void*) = nullptr;
};

class NLS_PLATFORM_API PlatformInputBackend final
{
public:
    PlatformInputBackend(Input::InputSystem& input, Window& mainWindow);
    ~PlatformInputBackend();
    PlatformInputBackend(const PlatformInputBackend&) = delete;
    PlatformInputBackend& operator=(const PlatformInputBackend&) = delete;

    Input::WindowId GetMainWindowId() const { return m_mainWindow; }
    Input::WindowId RegisterGlfwWindow(GLFWwindow* window, InputForwardingContext forwarding = {});
#ifdef _WIN32
    Input::WindowId RegisterWin32Window(void* hwnd, InputForwardingContext forwarding = {});
    void HandleNativeTextMessage(void* hwnd, unsigned int message, long long parameter);
#endif
    bool SetTextInputRect(Input::WindowId window, Maths::Vector2 screenPosition, float lineHeight);
    bool SetPointerCapture(bool capture, const void* owner);
    Maths::Vector2 PollPointerWrap();
    Input::WindowId GetPointerCaptureWindow() const { return m_captureWindow; }
    bool OwnsPointerCapture(const void* owner) const { return m_captureWindow && m_captureOwner == owner; }
    bool UnregisterWindow(Input::WindowId window);
    Input::WindowId FindWindowId(uintptr_t nativeHandle) const;
    void AttachWindowContext(Input::WindowId window, InputForwardingContext forwarding);
    void DetachWindowContext(Input::WindowId window, void* context);
    InputForwardingContext GetWindowContext(uintptr_t nativeHandle) const;
    void SubmitLegacy(const WindowInputEvent& event);
    void PollDevices();
    Input::DeviceId GetGamepad(size_t slot) const;
    TextInputService& GetTextInput() { return m_text; }
    const TextInputService& GetTextInput() const { return m_text; }
    Input::InputSystem& GetInputSystem() const { return m_input; }
    std::weak_ptr<const void> GetLifetime() const { return m_lifetime; }

private:
    struct Registration;
    static std::unordered_map<GLFWwindow*, Registration*>& GlfwRegistrations();
    static Registration* Lookup(GLFWwindow* window);
    void InstallGlfwCallbacks(Registration& registration);
    void RestoreGlfwCallbacks(Registration& registration);
    void Receive(Registration& registration, WindowInputEvent event);
#ifdef _WIN32
    void ReconcileReleasedControls();
#endif
    Input::InputSystem& m_input;
    Window& m_window;
    Input::WindowId m_mainWindow;
    Input::WindowId m_captureWindow;
    const void* m_captureOwner = nullptr;
    ListenerID m_pollListener = 0;
    std::vector<std::unique_ptr<Registration>> m_registrations;
    std::array<Input::DeviceId, 16> m_gamepads{};
    TextInputService m_text;
    std::shared_ptr<const void> m_lifetime = std::make_shared<int>(0);
};
}
