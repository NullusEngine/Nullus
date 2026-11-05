#pragma once
#include <memory>
#include <unordered_map>
#include "Windowing/Window.h"
#include "Windowing/Inputs/EKey.h"
#include "Windowing/Inputs/EKeyState.h"
#include "Windowing/Inputs/EMouseButton.h"
#include "Windowing/Inputs/EMouseButtonState.h"
#include "PlatformInputBackend.h"

namespace NLS::Windowing::Inputs
{
class NLS_PLATFORM_API InputManager
{
public:
    explicit InputManager(Window& window);
    InputManager(Window& window, Input::InputSystem& input);
    ~InputManager();
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;
    void Update();
    EKeyState GetKeyState(EKey key) const;
    EMouseButtonState GetMouseButtonState(EMouseButton button) const;
    bool IsKeyPressed(EKey key) const;
    bool IsKeyReleased(EKey key) const;
    bool IsMouseButtonPressed(EMouseButton button) const;
    bool IsMouseButtonReleased(EMouseButton button) const;
    bool IsKeyDown(EKey key) const { return GetKeyState(key) == EKeyState::KEY_DOWN; }
    bool WasKeyPressed(EKey key) const { return IsKeyPressed(key); }
    bool WasKeyReleased(EKey key) const { return IsKeyReleased(key); }
    Maths::Vector2 GetMousePosition() const;
    Maths::Vector2 GetMouseDelta() const;
    Maths::Vector2 GetMouseWheel() const;
    Maths::Vector2 GetWheelMovement() const { return GetMouseWheel(); }
    Maths::Vector2 PollInfiniteCursorWrap();
    bool HasFocusedWindow() const;
    bool SetPointerCapture(bool capture, const void* owner) { return m_backend->SetPointerCapture(capture, owner); }
    Input::GamepadState GetGamepadState(size_t slot = 0) const;
    const std::string& GetTextInput() const;
    const std::unordered_map<EKey, EKeyState>& GetKeyEvents() const;
    bool HasTransientInputEvents() const;
    void ClearEvents();
    Input::InputSystem& GetInputSystem() const { return *m_input; }
    PlatformInputBackend& GetPlatformBackend() const { return *m_backend; }
private:
    void BindLegacyEvents();
    void SubmitLegacy(WindowInputEventType type, int key = 0, double x = 0, double y = 0);
    Window& m_window;
    std::unique_ptr<Input::InputSystem> m_ownedInput;
    Input::InputSystem* m_input = nullptr;
    std::unique_ptr<PlatformInputBackend> m_backend;
    ListenerID m_keyPressedListener{}, m_keyReleasedListener{}, m_characterListener{};
    ListenerID m_mouseButtonPressedListener{}, m_mouseButtonReleasedListener{}, m_mouseScrollListener{};
    ListenerID m_cursorListener{}, m_focusGainedListener{}, m_focusLostListener{};
    mutable std::unordered_map<EKey, EKeyState> m_legacyKeyEvents;
    mutable uint64_t m_legacySequence = 0;
};
}
