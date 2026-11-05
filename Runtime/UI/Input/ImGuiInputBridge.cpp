#include "ImGuiInputBridge.h"
#include <Platform/Windowing/Inputs/PlatformInputBackend.h>
#include <ImGui/imgui.h>
#include <unordered_map>
#include <vector>
#include <stdexcept>

namespace NLS::UI
{
struct ImGuiInputBridge::State
{
    Windowing::Inputs::PlatformInputBackend* input;
    std::weak_ptr<const void> lifetime;
    ImGuiContext* context;
    bool nativeWin32;
    Input::WindowId mainWindow;
    void (*create)(ImGuiViewport*) = nullptr;
    void (*destroy)(ImGuiViewport*) = nullptr;
    void (*ime)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*) = nullptr;
    struct Entry { ImGuiViewport* viewport; Input::WindowId id; };
    std::vector<Entry> windows;

    static auto& Instances()
    {
        static std::unordered_map<ImGuiContext*, State*> instances;
        return instances;
    }
    static void* Activate(void* context)
    {
        auto* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
        return previous;
    }
    static void Restore(void* context) { ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context)); }
    void Register(ImGuiViewport* viewport)
    {
        if (lifetime.expired() || viewport == ImGui::GetMainViewport()) return;
        Input::WindowId id;
        if (nativeWin32)
        {
#ifdef _WIN32
            id = input->RegisterWin32Window(viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle, {context, Activate, Restore});
#endif
        }
        else
            id = input->RegisterGlfwWindow(static_cast<GLFWwindow*>(viewport->PlatformHandle), {context, Activate, Restore});
        if (id && id != input->GetMainWindowId()) windows.push_back({viewport, id});
    }
    void Unregister(ImGuiViewport* viewport)
    {
        for (auto it = windows.begin(); it != windows.end(); ++it)
            if (it->viewport == viewport)
            {
                if (!lifetime.expired()) input->UnregisterWindow(it->id);
                windows.erase(it);
                break;
            }
    }
    static void Create(ImGuiViewport* viewport)
    {
        auto& state = *Instances().at(ImGui::GetCurrentContext());
        state.create(viewport);
        state.Register(viewport);
    }
    static void Destroy(ImGuiViewport* viewport)
    {
        auto& state = *Instances().at(ImGui::GetCurrentContext());
        state.Unregister(viewport);
        state.destroy(viewport);
    }
    static void Ime(ImGuiContext* context, ImGuiViewport* viewport, ImGuiPlatformImeData* data)
    {
        auto& state = *Instances().at(context);
        if (state.ime) state.ime(context, viewport, data);
        if (!data->WantVisible || state.lifetime.expired()) return;
        const auto handle = reinterpret_cast<uintptr_t>(viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
        const auto window = state.input->FindWindowId(handle);
        if (window) state.input->SetTextInputRect(window, {data->InputPos.x, data->InputPos.y}, data->InputLineHeight);
    }
};

ImGuiInputBridge::ImGuiInputBridge(Windowing::Inputs::PlatformInputBackend& input, ImGuiContext* context, bool nativeWin32, Input::WindowId mainWindow)
    : m_state(std::make_unique<State>())
{
    if (!context || State::Instances().contains(context)) throw std::invalid_argument("Input bridge requires a unique ImGui context");
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetPlatformIO();
    auto& state = *m_state;
    state.input = &input;
    state.lifetime = input.GetLifetime();
    state.context = context;
    state.nativeWin32 = nativeWin32;
    state.mainWindow = mainWindow ? mainWindow : input.GetMainWindowId();
    input.AttachWindowContext(state.mainWindow, {context, State::Activate, State::Restore});
    state.create = io.Platform_CreateWindow;
    state.destroy = io.Platform_DestroyWindow;
    state.ime = ImGui::GetIO().PlatformSetImeDataFn;
    ImGui::GetIO().PlatformSetImeDataFn = State::Ime;
    State::Instances().emplace(context, &state);
    if (state.create && state.destroy)
    {
        io.Platform_CreateWindow = State::Create;
        io.Platform_DestroyWindow = State::Destroy;
        for (auto* viewport : io.Viewports)
            if (viewport->PlatformHandle) state.Register(viewport);
    }
    ImGui::SetCurrentContext(previous);
}

ImGuiInputBridge::~ImGuiInputBridge()
{
    auto& state = *m_state;
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(state.context);
    auto& io = ImGui::GetPlatformIO();
    if (io.Platform_CreateWindow == State::Create) io.Platform_CreateWindow = state.create;
    if (io.Platform_DestroyWindow == State::Destroy) io.Platform_DestroyWindow = state.destroy;
    if (ImGui::GetIO().PlatformSetImeDataFn == State::Ime) ImGui::GetIO().PlatformSetImeDataFn = state.ime;
    if (!state.lifetime.expired())
        for (const auto& entry : state.windows) state.input->UnregisterWindow(entry.id);
    if (!state.lifetime.expired()) state.input->DetachWindowContext(state.mainWindow, state.context);
    State::Instances().erase(state.context);
    ImGui::SetCurrentContext(previous);
}
}
