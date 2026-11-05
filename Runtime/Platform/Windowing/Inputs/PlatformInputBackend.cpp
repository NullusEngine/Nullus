#include "PlatformInputBackend.h"
#include "Windowing/Window.h"
#include "Windowing/InfiniteCursorWrapState.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <windowsx.h>
#include <imm.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace NLS::Windowing::Inputs
{
#ifndef _WIN32
bool PlatformInputBackend::SetTextInputRect(Input::WindowId, Maths::Vector2, float)
{ return false; } // Composition/candidate positioning is not provided by this backend on these platforms.
#endif
namespace
{
auto& HostCollectors()
{
    static std::unordered_map<Input::InputSystem*, PlatformInputBackend*> collectors;
    return collectors;
}
uint32_t LogicalCodepoint(const char* name)
{
    if (!name || !*name) return 0;
    const auto first = static_cast<unsigned char>(*name++);
    if (first > 0xf4) return 0;
    if (first < 0x80) return *name ? 0 : first;
    int remaining = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : first >= 0xc2 ? 1 : -1;
    if (remaining < 0) return 0;
    const uint32_t minimum = remaining == 1 ? 0x80u : remaining == 2 ? 0x800u : 0x10000u;
    uint32_t code = first & (remaining == 1 ? 0x1fu : remaining == 2 ? 0x0fu : 0x07u);
    while (remaining--)
    {
        const auto next = static_cast<unsigned char>(*name++);
        if ((next & 0xc0u) != 0x80u) return 0;
        code = (code << 6u) | (next & 0x3fu);
    }
    return *name || code < minimum || code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu) ? 0 : code;
}
}
struct PlatformInputBackend::Registration
{
    PlatformInputBackend* backend = nullptr;
    Input::WindowId id;
    uintptr_t nativeHandle = 0;
    GLFWwindow* glfwWindow = nullptr;
    Window* mainWindow = nullptr;
    Window* ownerWindow = nullptr;
    ListenerID listener = 0;
    bool focusLost = false;
    bool callbacksWrapped = false;
    InfiniteCursorWrapState wrap;
    Maths::Vector2 warpTarget;
    bool pendingWarp = false;
    InputForwardingContext forwarding;
    struct ContextScope
    {
        InputForwardingContext hooks;
        void* previous;
        explicit ContextScope(InputForwardingContext value) : hooks(value),
            previous(value.activate ? value.activate(value.context) : nullptr) {}
        ~ContextScope() { if (hooks.restore) hooks.restore(previous); }
    };
    GLFWkeyfun key = nullptr;
    GLFWcharfun character = nullptr;
    GLFWcursorposfun cursor = nullptr;
    GLFWmousebuttonfun mouse = nullptr;
    GLFWscrollfun scroll = nullptr;
    GLFWwindowfocusfun focus = nullptr;
#ifdef _WIN32
    WNDPROC originalProc = nullptr;
    char16_t highSurrogate = 0;
    std::array<int, 512> scanToKey{};
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
#endif
};

std::unordered_map<GLFWwindow*, PlatformInputBackend::Registration*>& PlatformInputBackend::GlfwRegistrations()
{
    static std::unordered_map<GLFWwindow*, Registration*> registrations;
    return registrations;
}
PlatformInputBackend::Registration* PlatformInputBackend::Lookup(GLFWwindow* window)
{
    const auto found = GlfwRegistrations().find(window);
    return found == GlfwRegistrations().end() ? nullptr : found->second;
}
PlatformInputBackend::PlatformInputBackend(Input::InputSystem& input, Window& mainWindow)
    : m_input(input), m_window(mainWindow)
{
    if (mainWindow.GetInputBackend()) throw std::logic_error("A window already has an input collector.");
    if (HostCollectors().contains(&input)) throw std::logic_error("An input host already has a platform collector. Register additional windows with that collector.");
    auto registration = std::make_unique<Registration>();
    registration->backend = this;
    registration->mainWindow = &mainWindow;
    registration->ownerWindow = &mainWindow;
    registration->glfwWindow = mainWindow.GetGlfwWindow();
    registration->nativeHandle = reinterpret_cast<uintptr_t>(mainWindow.GetNativeWindowHandle());
    if (!registration->nativeHandle) registration->nativeHandle = reinterpret_cast<uintptr_t>(registration->glfwWindow);
    registration->id = input.RegisterWindow(registration->nativeHandle, Input::IngressKind::Glfw, mainWindow.IsFocused());
    if (!registration->id) throw std::logic_error("Conflicting input ingress for the main window.");
    m_mainWindow = registration->id;
    auto* raw = registration.get();
    registration->listener = mainWindow.InputEvent.AddListener([raw](const WindowInputEvent& event)
    {
        raw->backend->Receive(*raw, event);
    });
    m_registrations.push_back(std::move(registration));
    mainWindow.SetInputBackend(this);
#ifdef _WIN32
    mainWindow.InstallNativeWindowProc();
#endif
    m_pollListener = mainWindow.GetDevice()->InputPollCompletedEvent.AddListener([this] { PollDevices(); });
    WindowInputEvent position;
    position.type = WindowInputEventType::PointerMove;
    position.pointerWarp = true;
    glfwGetCursorPos(mainWindow.GetGlfwWindow(), &position.x, &position.y);
    SubmitLegacy(position);
    HostCollectors().emplace(&input, this);
}
PlatformInputBackend::~PlatformInputBackend()
{
    m_window.GetDevice()->InputPollCompletedEvent.RemoveListener(m_pollListener);
    while (!m_registrations.empty()) UnregisterWindow(m_registrations.back()->id);
    if (m_window.GetInputBackend() == this) m_window.SetInputBackend(nullptr);
    HostCollectors().erase(&m_input);
}
Input::WindowId PlatformInputBackend::FindWindowId(uintptr_t handle) const
{
    if (!handle) return {};
    for (const auto& registration : m_registrations)
        if (registration->nativeHandle == handle || reinterpret_cast<uintptr_t>(registration->glfwWindow) == handle) return registration->id;
    return {};
}

void PlatformInputBackend::Receive(Registration& registration, WindowInputEvent event)
{
    if (event.type == WindowInputEventType::FocusLost && registration.id == m_captureWindow) SetPointerCapture(false, nullptr);
    if (event.type == WindowInputEventType::PointerMove && !event.pointerWarp && registration.pendingWarp &&
        std::abs(event.x - registration.warpTarget.x) < .5 && std::abs(event.y - registration.warpTarget.y) < .5)
    { event.pointerWarp = true; registration.pendingWarp = false; }
    using Kind = WindowInputEventType;
    using Type = Input::InputEventType;
    if (event.type == Kind::Character)
    {
        m_text.Commit(static_cast<char32_t>(event.character), registration.id, m_input.AllocateSequence(), m_input.Now());
        return;
    }
    Input::InputEvent raw;
    raw.window = registration.id;
    const auto* window = m_input.GetWindowState(registration.id);
    if (!window) return;
    raw.ingress = window->ingress;
    raw.scanCode = event.scanCode;
    raw.modifiers = event.modifiers;
    raw.x = event.x; raw.y = event.y;
    switch (event.type)
    {
    case Kind::KeyDown: raw.type = Type::ButtonDown; break;
    case Kind::KeyUp: raw.type = Type::ButtonUp; break;
    case Kind::KeyRepeat: raw.type = Type::ButtonRepeat; break;
    case Kind::MouseDown: raw.type = Type::ButtonDown; break;
    case Kind::MouseUp: raw.type = Type::ButtonUp; break;
    case Kind::PointerMove: raw.type = Type::PointerMove; break;
    case Kind::Wheel: raw.type = Type::Wheel; break;
    case Kind::FocusGained: raw.type = Type::FocusGained; registration.focusLost = false; break;
    case Kind::FocusLost: raw.type = Type::FocusLost; registration.focusLost = true; break;
    case Kind::CaptureLost: raw.type = Type::CaptureLost; break;
    default: return;
    }
    if (event.type == Kind::KeyDown || event.type == Kind::KeyUp || event.type == Kind::KeyRepeat)
    {
        if (event.key < 0 || event.key >= 512) return;
        raw.device = m_input.GetKeyboard();
        raw.control = static_cast<uint16_t>(event.key);
#ifdef _WIN32
        // Retain physical state during an internal focus transfer. The native
        // release reconciler covers GLFW's per-window key-up suppression.
        if (event.type == Kind::KeyUp && registration.focusLost) raw.flags = Input::InputEventFlags::FocusReset;
#endif
        raw.logicalKey = LogicalCodepoint(glfwGetKeyName(event.key, event.scanCode));
    }
    else if (event.type == Kind::MouseDown || event.type == Kind::MouseUp)
    {
        if (event.key < 0 || event.key >= 8) return;
        raw.device = m_input.GetMouse();
        raw.control = static_cast<uint16_t>(event.key);
    }
    else if (event.type == Kind::PointerMove || event.type == Kind::Wheel || event.type == Kind::CaptureLost)
        raw.device = m_input.GetMouse();
    if (event.type == Kind::PointerMove)
    {
        raw.flags = static_cast<Input::InputEventFlags>(4u | (event.pointerWarp ? 2u : 0u));
        int x = 0, y = 0;
        if (registration.glfwWindow) glfwGetWindowPos(registration.glfwWindow, &x, &y);
#ifdef _WIN32
        else
        {
            POINT origin{};
            ClientToScreen(reinterpret_cast<HWND>(registration.nativeHandle), &origin);
            x = origin.x; y = origin.y;
        }
#endif
        raw.screenX = event.x + x;
        raw.screenY = event.y + y;
    }
    m_input.SubmitEvent(raw);
}

void PlatformInputBackend::SubmitLegacy(const WindowInputEvent& event)
{
    for (const auto& registration : m_registrations)
        if (registration->id == m_mainWindow) { Receive(*registration, event); return; }
}

Input::WindowId PlatformInputBackend::RegisterGlfwWindow(GLFWwindow* window, InputForwardingContext forwarding)
{
    if (!window) return {};
    if (const auto existing = FindWindowId(reinterpret_cast<uintptr_t>(window))) return existing;
    if (Lookup(window)) throw std::logic_error("A GLFW window cannot have two input collectors.");
    auto* ownerWindow = Window::FindInstance(window);
    if (ownerWindow && ownerWindow->GetInputBackend() && ownerWindow->GetInputBackend() != this)
        throw std::logic_error("A GLFW window already belongs to a different input collector.");
    auto record = std::make_unique<Registration>();
    record->backend = this;
    record->glfwWindow = window;
    record->ownerWindow = ownerWindow;
    record->nativeHandle = reinterpret_cast<uintptr_t>(window);
#ifdef _WIN32
    record->nativeHandle = reinterpret_cast<uintptr_t>(glfwGetWin32Window(window));
#endif
    record->forwarding = forwarding;
    record->id = m_input.RegisterWindow(record->nativeHandle, Input::IngressKind::Glfw,
        glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE);
    if (!record->id) return {};
    InstallGlfwCallbacks(*record);
    const auto id = record->id;
    if (ownerWindow)
    {
        ownerWindow->SetInputBackend(this);
        auto* registered = record.get();
        record->listener = ownerWindow->InputEvent.AddListener([registered](const WindowInputEvent& event) { registered->backend->Receive(*registered,event); });
    }
#ifdef _WIN32
    if (ownerWindow) ownerWindow->InstallNativeWindowProc();
    else
    {
        const auto hwnd = reinterpret_cast<HWND>(record->nativeHandle);
        if (!SetPropW(hwnd, L"Nullus.InputCollector", record.get())) throw std::runtime_error("Cannot observe IME for a GLFW child window");
        record->originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Registration::WindowProc)));
    }
#endif
    m_registrations.push_back(std::move(record));
    return id;
}

void PlatformInputBackend::InstallGlfwCallbacks(Registration& registration)
{
    auto* record = &registration;
    auto* window = record->glfwWindow;
    if (!window || record->callbacksWrapped) return;
    record->callbacksWrapped = true;
    GlfwRegistrations().emplace(window, record);
    record->key = glfwSetKeyCallback(window, [](GLFWwindow* handle, int key, int scan, int action, int mods)
    {
        auto* r = Lookup(handle); if (!r) return;
        WindowInputEvent event;
        event.type = action == GLFW_PRESS ? WindowInputEventType::KeyDown :
            action == GLFW_RELEASE ? WindowInputEventType::KeyUp : WindowInputEventType::KeyRepeat;
        event.key = key; event.scanCode = scan; event.modifiers = static_cast<unsigned int>(mods);
        Registration::ContextScope scope(r->forwarding);
        if (!r->ownerWindow) r->backend->Receive(*r, event);
        if (r->key) r->key(handle, key, scan, action, mods);
    });
    record->character = glfwSetCharCallback(window, [](GLFWwindow* handle, unsigned int value)
    {
        auto* r = Lookup(handle); if (!r) return;
        WindowInputEvent event; event.type = WindowInputEventType::Character; event.character = value;
        Registration::ContextScope scope(r->forwarding);
        if (!r->ownerWindow) r->backend->Receive(*r, event);
        if (r->character) r->character(handle, value);
    });
    record->cursor = glfwSetCursorPosCallback(window, [](GLFWwindow* handle, double x, double y)
    {
        auto* r = Lookup(handle); if (!r) return;
        WindowInputEvent event; event.type = WindowInputEventType::PointerMove; event.x = x; event.y = y;
        Registration::ContextScope scope(r->forwarding);
        if (!r->ownerWindow) r->backend->Receive(*r, event);
        if (r->cursor) r->cursor(handle, x, y);
    });
    record->mouse = glfwSetMouseButtonCallback(window, [](GLFWwindow* handle, int button, int action, int mods)
    {
        auto* r = Lookup(handle); if (!r) return;
        WindowInputEvent event; event.type = action == GLFW_PRESS ? WindowInputEventType::MouseDown : WindowInputEventType::MouseUp;
        event.key = button; event.modifiers = static_cast<unsigned int>(mods);
        Registration::ContextScope scope(r->forwarding);
        if (!r->ownerWindow) r->backend->Receive(*r, event);
        if (r->mouse) r->mouse(handle, button, action, mods);
    });
    record->scroll = glfwSetScrollCallback(window, [](GLFWwindow* handle, double x, double y)
    {
        auto* r = Lookup(handle); if (!r) return;
        WindowInputEvent event; event.type = WindowInputEventType::Wheel; event.x = x; event.y = y;
        Registration::ContextScope scope(r->forwarding);
        if (!r->ownerWindow) r->backend->Receive(*r, event);
        if (r->scroll) r->scroll(handle, x, y);
    });
    record->focus = glfwSetWindowFocusCallback(window, [](GLFWwindow* handle, int focused)
    {
        auto* r = Lookup(handle); if (!r) return;
        WindowInputEvent event; event.type = focused ? WindowInputEventType::FocusGained : WindowInputEventType::FocusLost;
        Registration::ContextScope scope(r->forwarding);
        if (!r->ownerWindow) r->backend->Receive(*r, event);
        if (r->focus) r->focus(handle, focused);
    });
}
void PlatformInputBackend::RestoreGlfwCallbacks(Registration& r)
{
    if (!r.callbacksWrapped) return;
    glfwSetKeyCallback(r.glfwWindow,r.key);glfwSetCharCallback(r.glfwWindow,r.character);
    glfwSetCursorPosCallback(r.glfwWindow,r.cursor);glfwSetMouseButtonCallback(r.glfwWindow,r.mouse);
    glfwSetScrollCallback(r.glfwWindow,r.scroll);glfwSetWindowFocusCallback(r.glfwWindow,r.focus);
    GlfwRegistrations().erase(r.glfwWindow);r.callbacksWrapped=false;
}
void PlatformInputBackend::AttachWindowContext(Input::WindowId window, InputForwardingContext forwarding)
{
    for (const auto& r : m_registrations) if (r->id == window)
    {
        if (r->forwarding.context && r->forwarding.context != forwarding.context) throw std::logic_error("A native window already belongs to an ImGui context");
        r->forwarding = forwarding;
        InstallGlfwCallbacks(*r);
        return;
    }
    throw std::invalid_argument("Input window is not registered");
}
void PlatformInputBackend::DetachWindowContext(Input::WindowId window, void* context)
{
    for (const auto& r : m_registrations) if (r->id == window && r->forwarding.context == context)
    { RestoreGlfwCallbacks(*r); r->forwarding = {}; return; }
}
InputForwardingContext PlatformInputBackend::GetWindowContext(uintptr_t nativeHandle) const
{
    if (!nativeHandle) return {};
    for (const auto& r : m_registrations) if (r->nativeHandle == nativeHandle || reinterpret_cast<uintptr_t>(r->glfwWindow) == nativeHandle) return r->forwarding;
    return {};
}

bool PlatformInputBackend::UnregisterWindow(Input::WindowId id)
{
    if (m_captureWindow == id) SetPointerCapture(false, nullptr);
    const auto found = std::find_if(m_registrations.begin(), m_registrations.end(),
        [id](const auto& value) { return value->id == id; });
    if (found == m_registrations.end()) return false;
    auto& r = **found;
    if (r.ownerWindow && r.listener) r.ownerWindow->InputEvent.RemoveListener(r.listener);
    RestoreGlfwCallbacks(r);
#ifdef _WIN32
    if (r.originalProc)
    {
        const auto hwnd = reinterpret_cast<HWND>(r.nativeHandle);
        if (IsWindow(hwnd))
        {
            if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == Registration::WindowProc)
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(r.originalProc));
            RemovePropW(hwnd, L"Nullus.InputCollector");
        }
    }
#endif
    if (r.ownerWindow && r.ownerWindow->GetInputBackend() == this) r.ownerWindow->SetInputBackend(nullptr);
    m_text.CancelComposition(id, m_input.AllocateSequence(), m_input.Now());
    m_input.UnregisterWindow(id);
    m_registrations.erase(found);
    return true;
}
bool PlatformInputBackend::SetPointerCapture(bool capture, const void* owner)
{
    if (!capture)
    {
        if (!m_captureWindow || (owner && owner != m_captureOwner)) return false;
        const auto previous = m_captureWindow; m_captureWindow = {}; m_captureOwner = nullptr;
        for (const auto& record : m_registrations) if (record->id == previous)
        {
            record->wrap.SetEnabled(false);
            if (record->ownerWindow) record->ownerWindow->SetInfiniteCursorWrapEnabled(false);
#ifdef _WIN32
            if (GetCapture() == reinterpret_cast<HWND>(record->nativeHandle)) ReleaseCapture();
#endif
        }
        return true;
    }
    if (m_captureWindow) return owner == m_captureOwner;
    auto window = m_input.GetPointerWindow();
    if (!m_input.GetWindowState(window)) window = m_mainWindow;
    for (const auto& record : m_registrations) if (record->id == window)
    {
        m_captureWindow = window; m_captureOwner = owner; record->wrap.SetEnabled(true);
        if (record->ownerWindow) record->ownerWindow->SetInfiniteCursorWrapEnabled(true);
#ifdef _WIN32
        SetCapture(reinterpret_cast<HWND>(record->nativeHandle));
#endif
        return true;
    }
    return false;
}
Maths::Vector2 PlatformInputBackend::PollPointerWrap()
{
    if (!m_captureWindow) return m_window.PollInfiniteCursorWrap();
    for (const auto& record : m_registrations) if (record->id == m_captureWindow)
    {
        const auto* state = m_input.GetWindowState(record->id);
        if (!state || !state->focused) { SetPointerCapture(false, nullptr); return {}; }
        if (record->ownerWindow) return record->ownerWindow->PollInfiniteCursorWrap();
        double x=0, y=0; int width=0, height=0;
        if (record->glfwWindow) { glfwGetCursorPos(record->glfwWindow,&x,&y); glfwGetWindowSize(record->glfwWindow,&width,&height); }
#ifdef _WIN32
        else
        {
            const auto hwnd = reinterpret_cast<HWND>(record->nativeHandle); POINT point{}; RECT rect{};
            GetCursorPos(&point); ScreenToClient(hwnd,&point); GetClientRect(hwnd,&rect);
            x=point.x; y=point.y; width=rect.right; height=rect.bottom;
        }
#endif
        record->wrap.BeginFrame(); record->wrap.SeedCursorPosition(x,y);
        const auto request=record->wrap.Evaluate({static_cast<float>(width),static_cast<float>(height)},true);
        if (!request) return {};
        record->pendingWarp=true;record->warpTarget=request->targetPosition;
        if (record->glfwWindow) glfwSetCursorPos(record->glfwWindow,record->warpTarget.x,record->warpTarget.y);
#ifdef _WIN32
        else
        { POINT point{static_cast<LONG>(record->warpTarget.x),static_cast<LONG>(record->warpTarget.y)}; ClientToScreen(reinterpret_cast<HWND>(record->nativeHandle),&point); SetCursorPos(point.x,point.y); }
#endif
        WindowInputEvent event;event.type=WindowInputEventType::PointerMove;event.pointerWarp=true;event.x=record->warpTarget.x;event.y=record->warpTarget.y;
        Receive(*record,event);
        return request->compensation;
    }
    return {};
}

Input::DeviceId PlatformInputBackend::GetGamepad(size_t slot) const
{
    return slot < m_gamepads.size() ? m_gamepads[slot] : Input::DeviceId{};
}
void PlatformInputBackend::PollDevices()
{
    for (int slot = GLFW_JOYSTICK_1; slot <= GLFW_JOYSTICK_LAST; ++slot)
    {
        auto& id = m_gamepads[static_cast<size_t>(slot)];
        if (glfwJoystickPresent(slot) != GLFW_TRUE)
        {
            if (id) { m_input.RemoveDevice(id); id = {}; }
            continue;
        }
        const bool mapped = glfwJoystickIsGamepad(slot) == GLFW_TRUE;
        const auto* previous = m_input.FindDevice(id);
        if (previous && (previous->descriptor.layout == "Gamepad") != mapped)
        {
            m_input.RemoveDevice(id); id = {};
        }
        if (!id)
        {
            const char* name = glfwGetJoystickName(slot);
            const char* guid = glfwGetJoystickGUID(slot);
            id = m_input.RegisterDevice({Input::DeviceKind::Gamepad, name ? name : "Joystick",
                mapped ? "Gamepad" : "UnmappedJoystick", guid ? guid : "", false});
        }
        GLFWgamepadstate state{};
        if (!mapped || glfwGetGamepadState(slot, &state) != GLFW_TRUE) continue;
        const auto* current = m_input.FindDevice(id);
        for (uint16_t button = 0; button <= GLFW_GAMEPAD_BUTTON_LAST; ++button)
        {
            const bool down = state.buttons[button] == GLFW_PRESS;
            if (current->buttons[button] == down) continue;
            Input::InputEvent event;
            event.ingress = Input::IngressKind::Glfw; event.device = id; event.control = button;
            event.type = down ? Input::InputEventType::ButtonDown : Input::InputEventType::ButtonUp;
            m_input.SubmitEvent(event);
        }
        for (uint16_t axis = 0; axis <= GLFW_GAMEPAD_AXIS_LAST; ++axis)
        {
            const float value = axis >= GLFW_GAMEPAD_AXIS_LEFT_TRIGGER ? (state.axes[axis] + 1.0f) * 0.5f : state.axes[axis];
            if (current->axes[axis] == value) continue;
            Input::InputEvent event;
            event.ingress = Input::IngressKind::Glfw; event.device = id; event.control = axis;
            event.type = Input::InputEventType::Axis; event.x = value;
            m_input.SubmitEvent(event);
        }
    }
#ifdef _WIN32
    ReconcileReleasedControls();
#endif
    m_input.FinishEventPump();
}

#ifdef _WIN32
void PlatformInputBackend::HandleNativeTextMessage(void* handle, unsigned int message, long long parameter)
{
    const auto window = FindWindowId(reinterpret_cast<uintptr_t>(handle));
    if (!window) return;
    if (message == WM_CAPTURECHANGED && window == m_captureWindow && GetCapture() != static_cast<HWND>(handle)) SetPointerCapture(false, nullptr);
    if (message == WM_IME_ENDCOMPOSITION || message == WM_KILLFOCUS)
    { m_text.CancelComposition(window, m_input.AllocateSequence(), m_input.Now()); return; }
    if (message == WM_IME_STARTCOMPOSITION)
    { m_text.UpdateComposition({}, 0, window, m_input.AllocateSequence(), m_input.Now()); return; }
    if (message != WM_IME_COMPOSITION) return;
    // The existing GLFW/Win32 character ingress owns committed text. Observing
    // GCS_RESULTSTR here would submit the same characters a second time.
    if (parameter & GCS_RESULTSTR) m_text.CancelComposition(window, m_input.AllocateSequence(), m_input.Now());
    if (!(parameter & GCS_COMPSTR)) return;
    const auto hwnd = static_cast<HWND>(handle);
    if (const auto context = ImmGetContext(hwnd))
    {
        const auto bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
        const auto nativeCaret = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
        if (bytes >= 0 && bytes <= 65536)
        {
            std::wstring native(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(context, GCS_COMPSTR, native.data(), static_cast<DWORD>(bytes));
            std::u32string text; text.reserve(native.size()); size_t caret = 0;
            for (size_t i = 0; i < native.size(); ++i)
            {
                if (nativeCaret >= 0 && i < static_cast<size_t>(nativeCaret)) ++caret;
                char32_t codepoint = native[i];
                if (codepoint >= 0xd800 && codepoint <= 0xdbff && i + 1 < native.size() && native[i+1] >= 0xdc00 && native[i+1] <= 0xdfff)
                { codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (native[++i] - 0xdc00); }
                text.push_back(codepoint);
            }
            m_text.UpdateComposition(std::move(text), caret, window, m_input.AllocateSequence(), m_input.Now());
        }
        ImmReleaseContext(hwnd, context);
    }
}
bool PlatformInputBackend::SetTextInputRect(Input::WindowId window, Maths::Vector2 screenPosition, float lineHeight)
{
    const auto* state = m_input.GetWindowState(window); if (!state) return false;
    const auto hwnd = reinterpret_cast<HWND>(state->nativeHandle);
    const auto context = ImmGetContext(hwnd); if (!context) return false;
    POINT point{static_cast<LONG>(screenPosition.x), static_cast<LONG>(screenPosition.y)}; ScreenToClient(hwnd, &point);
    COMPOSITIONFORM composition{}; composition.dwStyle = CFS_POINT; composition.ptCurrentPos = point;
    CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_EXCLUDE; candidate.ptCurrentPos = point;
    candidate.rcArea = {point.x, point.y, point.x + 1, point.y + static_cast<LONG>((std::max)(1.f, lineHeight))};
    const bool success = ImmSetCompositionWindow(context, &composition) && ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(hwnd, context); return success;
}
void PlatformInputBackend::ReconcileReleasedControls()
{
    // GLFW resets a losing window's keys and may suppress the actual up in a
    // newly focused window that never saw the down. Only reconcile controls
    // still held after callbacks, so an ordinary release is never injected twice.
    const auto focused = GetFocus();
    if (!focused || !FindWindowId(reinterpret_cast<uintptr_t>(focused))) return;
    const auto* keyboard = m_input.FindDevice(m_input.GetKeyboard());
    for (uint16_t key = 32; key <= GLFW_KEY_LAST; ++key)
    {
        if (!keyboard->buttons[key]) continue;
        const int scan = glfwGetKeyScancode(key);
        if (scan < 0) continue;
        const UINT nativeScan = (scan & 0x100) ? 0xe000u | (scan & 0xffu) : static_cast<UINT>(scan);
        const UINT virtualKey = MapVirtualKeyExW(nativeScan, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
        if (!virtualKey || (GetAsyncKeyState(static_cast<int>(virtualKey)) & 0x8000)) continue;
        Input::InputEvent event;
        event.type = Input::InputEventType::ButtonUp; event.device = m_input.GetKeyboard(); event.control = key;
        event.scanCode = scan; event.flags = Input::InputEventFlags::Resynchronize; event.ingress = Input::IngressKind::Glfw;
        m_input.SubmitEvent(event);
    }
    const auto* mouse = m_input.FindDevice(m_input.GetMouse());
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const int virtualKeys[] = {swapped ? VK_RBUTTON : VK_LBUTTON, swapped ? VK_LBUTTON : VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (uint16_t button = 0; button < 5; ++button)
        if (mouse->buttons[button] && !(GetAsyncKeyState(virtualKeys[button]) & 0x8000))
        {
            Input::InputEvent event;
            event.type = Input::InputEventType::ButtonUp; event.device = m_input.GetMouse(); event.control = button;
            event.flags = Input::InputEventFlags::Resynchronize; event.ingress = Input::IngressKind::Glfw;
            m_input.SubmitEvent(event);
        }
}
#endif

#ifdef _WIN32
Input::WindowId PlatformInputBackend::RegisterWin32Window(void* handle, InputForwardingContext forwarding)
{
    const auto hwnd = static_cast<HWND>(handle);
    if (!hwnd || !IsWindow(hwnd)) return {};
    if (const auto existing = FindWindowId(reinterpret_cast<uintptr_t>(hwnd))) return existing;
    if (GetPropW(hwnd, L"Nullus.InputCollector")) throw std::logic_error("A native window already has an input collector.");
    auto record = std::make_unique<Registration>();
    record->backend = this;
    record->nativeHandle = reinterpret_cast<uintptr_t>(hwnd);
    record->forwarding = forwarding;
    record->id = m_input.RegisterWindow(record->nativeHandle, Input::IngressKind::Win32, GetFocus() == hwnd);
    if (!record->id) return {};
    record->scanToKey.fill(GLFW_KEY_UNKNOWN);
    for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; ++key)
    {
        const bool valid = key == 32 || key == 39 || (key >= 44 && key <= 57) || key == 59 || key == 61 ||
            (key >= 65 && key <= 93) || key == 96 || key == 161 || key == 162 ||
            (key >= 256 && key <= 269) || (key >= 280 && key <= 284) || (key >= 290 && key <= 314) ||
            (key >= 320 && key <= 336) || (key >= 340 && key <= 348);
        if (!valid) continue;
        const int scan = glfwGetKeyScancode(key);
        if (scan >= 0 && scan < 512) record->scanToKey[static_cast<size_t>(scan)] = key;
    }
    auto* pointer = record.get();
    SetPropW(hwnd, L"Nullus.InputCollector", pointer);
    record->originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&Registration::WindowProc)));
    if (!record->originalProc)
    {
        RemovePropW(hwnd, L"Nullus.InputCollector");
        m_input.UnregisterWindow(record->id);
        return {};
    }
    const auto id = record->id;
    m_registrations.push_back(std::move(record));
    return id;
}

LRESULT CALLBACK PlatformInputBackend::Registration::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* r = static_cast<Registration*>(GetPropW(hwnd, L"Nullus.InputCollector"));
    if (!r) return DefWindowProcW(hwnd, message, wparam, lparam);
    Registration::ContextScope context(r->forwarding);
    r->backend->HandleNativeTextMessage(hwnd, message, static_cast<long long>(lparam));
    // GLFW owns physical controls and committed characters for this window.
    if (r->glfwWindow) return CallWindowProcW(r->originalProc, hwnd, message, wparam, lparam);
    WindowInputEvent event;
    bool submit = true;
    switch (message)
    {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
    {
        const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
        event.type = up ? WindowInputEventType::KeyUp :
            (lparam & (LPARAM{1} << 30)) ? WindowInputEventType::KeyRepeat : WindowInputEventType::KeyDown;
        const auto scan = static_cast<uint16_t>(((lparam >> 16) & 0xff) | ((lparam & (LPARAM{1} << 24)) ? 0x100 : 0));
        event.key = r->scanToKey[scan]; event.scanCode = scan;
        if (GetKeyState(VK_SHIFT) & 0x8000) event.modifiers |= GLFW_MOD_SHIFT;
        if (GetKeyState(VK_CONTROL) & 0x8000) event.modifiers |= GLFW_MOD_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000) event.modifiers |= GLFW_MOD_ALT;
        if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) event.modifiers |= GLFW_MOD_SUPER;
        break;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
        event.type = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN ?
            WindowInputEventType::MouseDown : WindowInputEventType::MouseUp;
        event.key = message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ? 0 :
            message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ? 1 :
            message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ? 2 : (GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4);
        break;
    case WM_MOUSEMOVE:
        event.type = WindowInputEventType::PointerMove; event.x = GET_X_LPARAM(lparam); event.y = GET_Y_LPARAM(lparam); break;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        event.type = WindowInputEventType::Wheel;
        if (message == WM_MOUSEWHEEL) event.y = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        else event.x = -static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        break;
    case WM_SETFOCUS: event.type = WindowInputEventType::FocusGained; break;
    case WM_KILLFOCUS: event.type = WindowInputEventType::FocusLost; break;
    case WM_CAPTURECHANGED: event.type = WindowInputEventType::CaptureLost; break;
    case WM_CHAR:
    {
        event.type = WindowInputEventType::Character;
        const auto unit = static_cast<char16_t>(wparam);
        if (unit >= 0xd800 && unit <= 0xdbff) { r->highSurrogate = unit; submit = false; }
        else if (unit >= 0xdc00 && unit <= 0xdfff)
        {
            event.character = r->highSurrogate ? 0x10000u + ((r->highSurrogate - 0xd800u) << 10u) + (unit - 0xdc00u) : 0xfffdu;
            r->highSurrogate = 0;
        }
        else { event.character = unit; r->highSurrogate = 0; }
        break;
    }
    default: submit = false; break;
    }
    if (submit) r->backend->Receive(*r, event);
    // The saved ImGui platform WndProc already forwards to ImGui. Call it once.
    return CallWindowProcW(r->originalProc, hwnd, message, wparam, lparam);
}
#endif
}
