#include "InputManager.h"
namespace NLS::Windowing::Inputs
{
InputManager::InputManager(Window& window) : m_window(window),
    m_ownedInput(std::make_unique<Input::InputSystem>()), m_input(m_ownedInput.get()),
    m_backend(std::make_unique<PlatformInputBackend>(*m_input, window)) { BindLegacyEvents(); }
InputManager::InputManager(Window& window, Input::InputSystem& input) : m_window(window), m_input(&input),
    m_backend(std::make_unique<PlatformInputBackend>(input, window)) { BindLegacyEvents(); }
void InputManager::BindLegacyEvents()
{
    // Window emits rich input first. Direct legacy Event::Invoke is supported,
    // while a native event is normalized exactly once.
    m_keyPressedListener = m_window.KeyPressedEvent.AddListener([this](int key) { SubmitLegacy(WindowInputEventType::KeyDown, key); });
    m_keyReleasedListener = m_window.KeyReleasedEvent.AddListener([this](int key) { SubmitLegacy(WindowInputEventType::KeyUp, key); });
    m_mouseButtonPressedListener = m_window.MouseButtonPressedEvent.AddListener([this](int key) { SubmitLegacy(WindowInputEventType::MouseDown, key); });
    m_mouseButtonReleasedListener = m_window.MouseButtonReleasedEvent.AddListener([this](int key) { SubmitLegacy(WindowInputEventType::MouseUp, key); });
    m_mouseScrollListener = m_window.MouseScrollEvent.AddListener([this](double x, double y) { SubmitLegacy(WindowInputEventType::Wheel, 0, x, y); });
    m_cursorListener = m_window.CursorMoveEvent.AddListener([this](int16_t x, int16_t y) { SubmitLegacy(WindowInputEventType::PointerMove, 0, x, y); });
    m_focusGainedListener = m_window.GainFocusEvent.AddListener([this]() { SubmitLegacy(WindowInputEventType::FocusGained); });
    m_focusLostListener = m_window.LostFocusEvent.AddListener([this]() { SubmitLegacy(WindowInputEventType::FocusLost); });
    m_characterListener = m_window.CharacterEvent.AddListener([this](unsigned int value)
    {
        if (m_window.IsDispatchingInputEvent()) return;
        WindowInputEvent event; event.type = WindowInputEventType::Character; event.character = value;
        m_backend->SubmitLegacy(event);
    });
}
InputManager::~InputManager()
{
    m_window.KeyPressedEvent.RemoveListener(m_keyPressedListener);
    m_window.KeyReleasedEvent.RemoveListener(m_keyReleasedListener);
    m_window.MouseButtonPressedEvent.RemoveListener(m_mouseButtonPressedListener);
    m_window.MouseButtonReleasedEvent.RemoveListener(m_mouseButtonReleasedListener);
    m_window.MouseScrollEvent.RemoveListener(m_mouseScrollListener);
    m_window.CursorMoveEvent.RemoveListener(m_cursorListener);
    m_window.GainFocusEvent.RemoveListener(m_focusGainedListener);
    m_window.LostFocusEvent.RemoveListener(m_focusLostListener);
    m_window.CharacterEvent.RemoveListener(m_characterListener);
}
void InputManager::SubmitLegacy(WindowInputEventType type, int key, double x, double y)
{
    if (m_window.IsDispatchingInputEvent()) return;
    WindowInputEvent event; event.type = type; event.key = key; event.x = x; event.y = y;
    m_backend->SubmitLegacy(event);
}
void InputManager::Update() { m_backend->PollDevices(); }
EKeyState InputManager::GetKeyState(EKey key) const
{
    const int code = static_cast<int>(key);
    return code >= 0 && code < 512 && m_input->IsButtonDown(m_input->GetKeyboard(), static_cast<uint16_t>(code)) ? EKeyState::KEY_DOWN : EKeyState::KEY_UP;
}
EMouseButtonState InputManager::GetMouseButtonState(EMouseButton button) const
{
    const int code = static_cast<int>(button);
    return code >= 0 && code < 8 && m_input->IsButtonDown(m_input->GetMouse(), static_cast<uint16_t>(code)) ? EMouseButtonState::MOUSE_DOWN : EMouseButtonState::MOUSE_UP;
}
bool InputManager::IsKeyPressed(EKey key) const
{
    const int code = static_cast<int>(key);
    return code >= 0 && code < 512 && m_input->WasButtonPressed(m_input->GetKeyboard(), static_cast<uint16_t>(code));
}
bool InputManager::IsKeyReleased(EKey key) const
{
    const int code = static_cast<int>(key);
    return code >= 0 && code < 512 && m_input->WasButtonReleased(m_input->GetKeyboard(), static_cast<uint16_t>(code));
}
bool InputManager::IsMouseButtonPressed(EMouseButton button) const
{
    const int code = static_cast<int>(button);
    return code >= 0 && code < 8 && m_input->WasButtonPressed(m_input->GetMouse(), static_cast<uint16_t>(code));
}
bool InputManager::IsMouseButtonReleased(EMouseButton button) const
{
    const int code = static_cast<int>(button);
    return code >= 0 && code < 8 && m_input->WasButtonReleased(m_input->GetMouse(), static_cast<uint16_t>(code));
}
Maths::Vector2 InputManager::GetMousePosition() const
{
    const auto* mouse = m_input->FindDevice(m_input->GetMouse());
    return mouse && mouse->hasScreenPosition ? m_input->GetPointerScreenPosition() - m_window.GetPosition() : m_input->GetMousePosition();
}
bool InputManager::HasFocusedWindow() const
{
    for (const auto& window : m_input->GetWindows()) if (window.registered && window.focused) return true;
    return false;
}
Maths::Vector2 InputManager::GetMouseDelta() const { return m_input->GetMouseDelta(); }
Maths::Vector2 InputManager::GetMouseWheel() const { return m_input->GetMouseWheel(); }
Maths::Vector2 InputManager::PollInfiniteCursorWrap() { return m_backend->PollPointerWrap(); }
Input::GamepadState InputManager::GetGamepadState(size_t slot) const { return m_input->GetGamepadState(m_backend->GetGamepad(slot)); }
const std::string& InputManager::GetTextInput() const { return m_backend->GetTextInput().GetCommittedText(); }
const std::unordered_map<EKey, EKeyState>& InputManager::GetKeyEvents() const
{
    if (m_legacySequence == m_input->GetLastSequence()) return m_legacyKeyEvents;
    m_legacyKeyEvents.clear();
    for (const auto& event : m_input->GetEvents())
        if (event.device == m_input->GetKeyboard() && (static_cast<unsigned int>(event.flags) & 8u) == 0)
        {
            if (event.type == Input::InputEventType::ButtonDown) m_legacyKeyEvents[static_cast<EKey>(event.control)] = EKeyState::KEY_DOWN;
            if (event.type == Input::InputEventType::ButtonUp) m_legacyKeyEvents[static_cast<EKey>(event.control)] = EKeyState::KEY_UP;
        }
    m_legacySequence = m_input->GetLastSequence();
    return m_legacyKeyEvents;
}
bool InputManager::HasTransientInputEvents() const { return m_input->HasTransientEvents() || !GetTextInput().empty(); }
void InputManager::ClearEvents()
{
    m_input->EndFrame();
    m_backend->GetTextInput().EndFrame();
    m_legacyKeyEvents.clear();
    m_legacySequence = m_input->GetLastSequence();
}
}
