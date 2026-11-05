#include <gtest/gtest.h>
#include <Platform/Windowing/Context/Device.h>
#include <Platform/Windowing/Inputs/InputManager.h>
#include <Platform/Windowing/Window.h>
#include <UI/Input/ImGuiInputBridge.h>
#include <ImGui/imgui.h>
#include <ImGui/imgui_internal.h>
#include <ImGui/backends/imgui_impl_glfw.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdlib>
#ifdef _WIN32
#include <Windows.h>
#include <ImGui/backends/imgui_impl_win32.h>
#endif

using namespace NLS;
TEST(TextInputServiceTests, CompositionDoesNotCommitAndCancelsOnlyItsOwningWindow)
{
    Windowing::Inputs::TextInputService text;
    const Input::WindowId first{1}, second{2};
    ASSERT_TRUE(text.UpdateComposition(U"中文", 1, first, 1, .1));
    EXPECT_TRUE(text.GetCommittedText().empty()); EXPECT_EQ(text.GetComposition().caret, 1u);
    EXPECT_FALSE(text.UpdateComposition(U"x", 2, first, 2, .2));
    text.CancelComposition(second, 3, .3); EXPECT_TRUE(text.GetComposition().active);
    text.EndFrame(); EXPECT_EQ(text.GetComposition().text, U"中文");
    ASSERT_TRUE(text.Commit(U'中', first, 4, .4)); EXPECT_EQ(text.GetEvents().size(), 1u);
    text.CancelComposition(first, 5, .5); EXPECT_FALSE(text.GetComposition().active);
}
namespace
{
class InputPlatformTests : public testing::Test
{
protected:
    void SetUp() override
    {
#if !defined(_WIN32) && !defined(__APPLE__)
        if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) GTEST_SKIP() << "A GLFW display is required";
#endif
        device = std::make_unique<Context::Device>(Windowing::Settings::DeviceSettings{});
        Windowing::Settings::WindowSettings settings;
        settings.visible = false;
        settings.width = settings.height = 64;
        settings.clientAPI = Windowing::Settings::WindowClientAPI::NoAPI;
        window = std::make_unique<Windowing::Window>(*device, settings);
        manager = std::make_unique<Windowing::Inputs::InputManager>(*window);
        manager->ClearEvents();
    }
    std::unique_ptr<Context::Device> device;
    std::unique_ptr<Windowing::Window> window;
    std::unique_ptr<Windowing::Inputs::InputManager> manager;
};
}

TEST_F(InputPlatformTests, RichAndLegacyCallbacksNormalizeOnlyOnce)
{
    auto* native = window->GetGlfwWindow();
    auto callback = glfwSetKeyCallback(native, nullptr);
    glfwSetKeyCallback(native, callback);
    ASSERT_NE(callback, nullptr);
    int legacyCalls = 0;
    auto listener = window->KeyPressedEvent.AddListener([&](int) { ++legacyCalls; });
    for (int i = 0; i < 3; ++i)
    {
        callback(native, GLFW_KEY_A, 30, GLFW_PRESS, 0);
        callback(native, GLFW_KEY_A, 30, GLFW_RELEASE, 0);
    }
    auto events = manager->GetInputSystem().GetEvents();
    EXPECT_EQ(events.size(), 6u);
    EXPECT_EQ(legacyCalls, 3);
    EXPECT_TRUE(manager->WasKeyPressed(Windowing::Inputs::EKey::KEY_A));
    EXPECT_TRUE(manager->WasKeyReleased(Windowing::Inputs::EKey::KEY_A));
    EXPECT_FALSE(manager->IsKeyDown(Windowing::Inputs::EKey::KEY_A));
    window->KeyPressedEvent.RemoveListener(listener);
}

TEST_F(InputPlatformTests, MainWindowCannotBeRegisteredThroughASecondIngress)
{
    auto& backend = manager->GetPlatformBackend();
    EXPECT_EQ(backend.RegisterGlfwWindow(window->GetGlfwWindow()), backend.GetMainWindowId());
#ifdef _WIN32
    EXPECT_EQ(backend.RegisterWin32Window(window->GetNativeWindowHandle()), backend.GetMainWindowId());
#endif
    EXPECT_EQ(manager->GetInputSystem().GetWindows().size(), 1u);
}
TEST_F(InputPlatformTests, HostCannotAccidentallyCreateASecondPhysicalDeviceCollector)
{
    Windowing::Settings::WindowSettings settings; settings.visible = false; settings.width = settings.height = 64;
    settings.clientAPI = Windowing::Settings::WindowClientAPI::NoAPI; Windowing::Window child(*device, settings);
    EXPECT_THROW((Windowing::Inputs::InputManager(child, manager->GetInputSystem())), std::logic_error);
    EXPECT_EQ(manager->GetInputSystem().GetDevices().size(), 2u);
    auto& backend = manager->GetPlatformBackend(); const auto id = backend.RegisterGlfwWindow(child.GetGlfwWindow());
    EXPECT_TRUE(id); EXPECT_TRUE(backend.UnregisterWindow(id));
}
TEST_F(InputPlatformTests, ChildCoordinatesAndPointerCaptureHaveOneExplicitOwner)
{
    auto* child=glfwCreateWindow(64,64,"Input child",nullptr,nullptr);ASSERT_NE(child,nullptr);
    auto& backend=manager->GetPlatformBackend();const auto childId=backend.RegisterGlfwWindow(child);
    auto cursor=glfwSetCursorPosCallback(child,nullptr);glfwSetCursorPosCallback(child,cursor);ASSERT_NE(cursor,nullptr);
    cursor(child,12,15);
    const auto screen=manager->GetInputSystem().GetPointerScreenPosition(), host=window->GetPosition();
    EXPECT_EQ(manager->GetMousePosition(),screen-host);
    int first=1, second=2;ASSERT_TRUE(backend.SetPointerCapture(true,&first));EXPECT_EQ(backend.GetPointerCaptureWindow(),childId);
    EXPECT_FALSE(backend.SetPointerCapture(false,&second));EXPECT_TRUE(backend.OwnsPointerCapture(&first));
    ASSERT_TRUE(backend.UnregisterWindow(childId));EXPECT_FALSE(backend.GetPointerCaptureWindow());
    glfwDestroyWindow(child);
}
TEST_F(InputPlatformTests, SeparateImGuiContextsReceiveOnlyTheirOwnWindowEvents)
{
    auto& backend=manager->GetPlatformBackend();
    auto* first=ImGui::CreateContext();ImGui_ImplGlfw_InitForOther(window->GetGlfwWindow(),true);
    {
        UI::ImGuiInputBridge firstBridge(backend,first,false);
        Windowing::Settings::WindowSettings settings;settings.visible=false;settings.width=settings.height=64;settings.clientAPI=Windowing::Settings::WindowClientAPI::NoAPI;
        Windowing::Window secondWindow(*device,settings);
        const auto id=backend.RegisterGlfwWindow(secondWindow.GetGlfwWindow());
        backend.DetachWindowContext(id,nullptr);
        auto* second=ImGui::CreateContext();ImGui::SetCurrentContext(second);ImGui_ImplGlfw_InitForOther(secondWindow.GetGlfwWindow(),true);
        {
            UI::ImGuiInputBridge secondBridge(backend,second,false,id);
            auto key=glfwSetKeyCallback(window->GetGlfwWindow(),nullptr);glfwSetKeyCallback(window->GetGlfwWindow(),key);
            manager->ClearEvents();key(window->GetGlfwWindow(),GLFW_KEY_A,30,GLFW_PRESS,0);
            EXPECT_EQ(ImGui::GetCurrentContext(),second);
            const auto count=[](ImGuiContext* context,ImGuiKey key){int result=0;for(const auto& event:context->InputEventsQueue)if(event.Type==ImGuiInputEventType_Key&&event.Key.Key==key&&event.Key.Down)++result;return result;};
            EXPECT_EQ(count(first,ImGuiKey_A),1);EXPECT_EQ(count(second,ImGuiKey_A),0);
            ImGui::SetCurrentContext(first);
            auto other=glfwSetKeyCallback(secondWindow.GetGlfwWindow(),nullptr);glfwSetKeyCallback(secondWindow.GetGlfwWindow(),other);
            other(secondWindow.GetGlfwWindow(),GLFW_KEY_B,48,GLFW_PRESS,0);
            EXPECT_EQ(ImGui::GetCurrentContext(),first);EXPECT_EQ(count(second,ImGuiKey_B),1);EXPECT_EQ(count(first,ImGuiKey_B),0);
            const auto events=manager->GetInputSystem().GetEvents();EXPECT_EQ(std::count_if(events.begin(),events.end(),[](const auto& event){return event.type==Input::InputEventType::ButtonDown;}),2);
        }
        ImGui::SetCurrentContext(second);ImGui_ImplGlfw_Shutdown();ImGui::DestroyContext(second);ImGui::SetCurrentContext(first);
        backend.UnregisterWindow(id);
    }
    ImGui::SetCurrentContext(first);ImGui_ImplGlfw_Shutdown();ImGui::DestroyContext(first);
}

TEST_F(InputPlatformTests, CursorWarpChangesPositionWithoutRelativeMovement)
{
    window->SetCursorPosition(12, 18);
    EXPECT_EQ(manager->GetMousePosition(), Maths::Vector2(12, 18));
    EXPECT_EQ(manager->GetMouseDelta(), Maths::Vector2(0, 0));
}

TEST_F(InputPlatformTests, ImGuiPlatformWindowForwardsInputOnceAndUnregistersBeforeDestroy)
{
    ImGui::CreateContext();
    auto* context = ImGui::GetCurrentContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ASSERT_TRUE(ImGui_ImplGlfw_InitForOther(window->GetGlfwWindow(), true));
    {
        UI::ImGuiInputBridge bridge(manager->GetPlatformBackend(), context, false);
        ImGuiViewport viewport;
        viewport.ID = 0xFF1234;
        viewport.Size = ImVec2(64, 64);
        viewport.Pos = ImVec2(20, 20);
        auto& platform = ImGui::GetPlatformIO();
        ASSERT_NE(platform.Platform_CreateWindow, nullptr);
        platform.Platform_CreateWindow(&viewport);
        auto* child = static_cast<GLFWwindow*>(viewport.PlatformHandle);
        ASSERT_NE(child, nullptr);
        auto callback = glfwSetKeyCallback(child, nullptr);
        glfwSetKeyCallback(child, callback);
        manager->ClearEvents();
        callback(child, GLFW_KEY_A, 30, GLFW_PRESS, 0);
        callback(child, GLFW_KEY_A, 30, GLFW_RELEASE, 0);
        EXPECT_EQ(manager->GetInputSystem().GetEvents().size(), 2u);
        const auto id = manager->GetPlatformBackend().FindWindowId(reinterpret_cast<uintptr_t>(child));
        ASSERT_TRUE(id);
        platform.Platform_DestroyWindow(&viewport);
        EXPECT_EQ(manager->GetInputSystem().GetWindowState(id), nullptr);
    }
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(context);
}

#ifdef _WIN32
TEST_F(InputPlatformTests, NativeImGuiChildUsesOnlyWin32IngressAndCommitsUnicodeOnce)
{
    ImGui::CreateContext();
    auto* context = ImGui::GetCurrentContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ASSERT_TRUE(ImGui_ImplWin32_Init(window->GetNativeWindowHandle()));
    {
        UI::ImGuiInputBridge bridge(manager->GetPlatformBackend(), context, true);
        ImGuiViewport viewport;
        viewport.ID = 0xAB1234;
        viewport.Size = ImVec2(64, 64);
        viewport.Pos = ImVec2(20, 20);
        auto& platform = ImGui::GetPlatformIO();
        platform.Platform_CreateWindow(&viewport);
        auto hwnd = static_cast<HWND>(viewport.PlatformHandleRaw);
        ASSERT_NE(hwnd, nullptr);
        manager->ClearEvents();
        SendMessageW(hwnd, WM_KEYDOWN, 'A', 1 | (30 << 16));
        SendMessageW(hwnd, WM_KEYUP, 'A', 1 | (30 << 16) | (1ll << 31));
        SendMessageW(hwnd, WM_CHAR, 0xD83D, 1);
        SendMessageW(hwnd, WM_CHAR, 0xDE00, 1);
        auto records = manager->GetInputSystem().GetEvents();
        EXPECT_EQ(std::count_if(records.begin(), records.end(), [](const auto& event)
        { return event.type == Input::InputEventType::ButtonDown || event.type == Input::InputEventType::ButtonUp; }), 2);
        EXPECT_EQ(manager->GetPlatformBackend().GetTextInput().GetEvents().size(), 1u);
        EXPECT_EQ(manager->GetPlatformBackend().GetTextInput().GetCommittedText(), "\xF0\x9F\x98\x80");
        platform.Platform_DestroyWindow(&viewport);
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(context);
}
#endif
