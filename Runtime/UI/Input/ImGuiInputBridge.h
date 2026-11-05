#pragma once
#include <Input/InputTypes.h>
#include <UI/UIDef.h>
#include <memory>

struct ImGuiContext;
namespace NLS::Windowing::Inputs { class PlatformInputBackend; }
namespace NLS::UI
{
// Hooks platform window creation, never ImGui's widget event processing.
class NLS_UI_API ImGuiInputBridge final
{
public:
    ImGuiInputBridge(Windowing::Inputs::PlatformInputBackend& input, ImGuiContext* context, bool nativeWin32, Input::WindowId mainWindow = {});
    ~ImGuiInputBridge();
    ImGuiInputBridge(const ImGuiInputBridge&) = delete;
    ImGuiInputBridge& operator=(const ImGuiInputBridge&) = delete;
private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
