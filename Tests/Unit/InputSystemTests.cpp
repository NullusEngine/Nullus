#include <gtest/gtest.h>
#include <Input/InputSystem.h>

using namespace NLS::Input;
TEST(InputSystemValidationTests, RejectedMouseEventsCannotChangePointerOwnership)
{
    InputSystem input; input.EndFrame(); const auto window = input.RegisterWindow(1, IngressKind::Virtual, true);
    InputEvent event; event.device = input.GetMouse(); event.window = window; event.type = InputEventType::ButtonDown; event.control = UINT16_MAX;
    EXPECT_FALSE(input.SubmitEvent(event)); EXPECT_FALSE(input.GetPointerWindow()); EXPECT_TRUE(input.GetEvents().empty());
    event.type = static_cast<InputEventType>(255); EXPECT_FALSE(input.SubmitEvent(event)); EXPECT_EQ(input.GetRejectedEventCount(), 2u);
}

namespace
{
InputEvent Button(DeviceId device, InputEventType type, uint16_t code, double time)
{
    InputEvent event;
    event.device = device;
    event.type = type;
    event.control = code;
    event.timestamp = time;
    return event;
}
}

TEST(InputSystemTests, PreservesEveryPressAndReleaseInOneFrame)
{
    double time = 0;
    InputSystem input([&] { return time; });
    input.EndFrame();
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(input.SubmitEvent(Button(input.GetKeyboard(), InputEventType::ButtonDown, 65, time += .01)));
        ASSERT_TRUE(input.SubmitEvent(Button(input.GetKeyboard(), InputEventType::ButtonUp, 65, time += .01)));
    }
    ASSERT_EQ(input.GetEvents().size(), 6u);
    EXPECT_TRUE(input.WasButtonPressed(input.GetKeyboard(), 65));
    EXPECT_TRUE(input.WasButtonReleased(input.GetKeyboard(), 65));
    EXPECT_FALSE(input.IsButtonDown(input.GetKeyboard(), 65));
    for (size_t i = 1; i < input.GetEvents().size(); ++i)
    {
        EXPECT_GT(input.GetEvents()[i].sequence, input.GetEvents()[i - 1].sequence);
        EXPECT_GT(input.GetEvents()[i].timestamp, input.GetEvents()[i - 1].timestamp);
    }
}

TEST(InputSystemTests, EndFrameClearsEdgesButPreservesHeldState)
{
    InputSystem input;
    ASSERT_TRUE(input.SubmitEvent(Button(input.GetKeyboard(), InputEventType::ButtonDown, 87, input.Now())));
    input.EndFrame();
    EXPECT_TRUE(input.IsButtonDown(input.GetKeyboard(), 87));
    EXPECT_FALSE(input.WasButtonPressed(input.GetKeyboard(), 87));
    EXPECT_FALSE(input.WasButtonReleased(input.GetKeyboard(), 87));
}

TEST(InputSystemTests, EventsAfterThePumpSurviveExactlyOneFrameBoundary)
{
    InputSystem input; input.EndFrame();
    ASSERT_TRUE(input.SubmitEvent(Button(input.GetKeyboard(), InputEventType::ButtonDown, 83, input.Now())));
    input.FinishEventPump();
    const auto processed = input.GetLastSequence();
    ASSERT_TRUE(input.SubmitEvent(Button(input.GetKeyboard(), InputEventType::ButtonUp, 83, input.Now())));
    for (int i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(input.SubmitEvent(Button(input.GetKeyboard(), InputEventType::ButtonDown, 83, input.Now())));
        ASSERT_TRUE(input.SubmitEvent(Button(input.GetKeyboard(), InputEventType::ButtonUp, 83, input.Now())));
    }
    EXPECT_FALSE(input.WasButtonReleased(input.GetKeyboard(), 83));
    input.EndFrame();
    ASSERT_EQ(input.GetEvents().size(), 5u);
    EXPECT_GT(input.GetEvents().front().sequence, processed);
    EXPECT_TRUE(input.WasButtonPressed(input.GetKeyboard(), 83));
    EXPECT_TRUE(input.WasButtonReleased(input.GetKeyboard(), 83));
    EXPECT_FALSE(input.IsButtonDown(input.GetKeyboard(), 83));
    input.FinishEventPump(); input.EndFrame();
    EXPECT_TRUE(input.GetEvents().empty());
    EXPECT_FALSE(input.WasButtonPressed(input.GetKeyboard(), 83));
    EXPECT_FALSE(input.WasButtonReleased(input.GetKeyboard(), 83));
}

TEST(InputSystemTests, DeferredPointerMotionAndWheelKeepOnlyTheirOwnDeltas)
{
    InputSystem input; input.EndFrame();
    InputEvent event; event.device = input.GetMouse(); event.type = InputEventType::PointerMove;
    event.x = 100; event.y = 100; ASSERT_TRUE(input.SubmitEvent(event));
    event.x = 110; event.y = 105; ASSERT_TRUE(input.SubmitEvent(event));
    input.FinishEventPump();
    event.x = 113; event.y = 109; ASSERT_TRUE(input.SubmitEvent(event));
    event.type = InputEventType::Wheel; event.x = 1; event.y = -2; ASSERT_TRUE(input.SubmitEvent(event));
    EXPECT_EQ(input.GetMouseDelta(), NLS::Maths::Vector2(10, 5));
    EXPECT_EQ(input.GetMouseWheel(), NLS::Maths::Vector2());
    input.EndFrame();
    EXPECT_EQ(input.GetMouseDelta(), NLS::Maths::Vector2(3, 4));
    EXPECT_EQ(input.GetMouseWheel(), NLS::Maths::Vector2(1, -2));
    EXPECT_EQ(input.GetEvents().size(), 2u);
    input.FinishEventPump(); input.EndFrame();
    EXPECT_EQ(input.GetMouseDelta(), NLS::Maths::Vector2());
    EXPECT_EQ(input.GetMouseWheel(), NLS::Maths::Vector2());
}

TEST(InputSystemTests, AccumulatesWheelAndDoesNotTurnWarpsIntoMotion)
{
    InputSystem input;
    InputEvent event;
    event.device = input.GetMouse();
    event.type = InputEventType::PointerMove;
    event.x = 100; event.y = 100;
    ASSERT_TRUE(input.SubmitEvent(event));
    event.x = 110; event.y = 105;
    ASSERT_TRUE(input.SubmitEvent(event));
    event.flags = InputEventFlags::PointerWarp;
    event.x = 0; event.y = 0;
    ASSERT_TRUE(input.SubmitEvent(event));
    EXPECT_EQ(input.GetMouseDelta().x, 10);
    EXPECT_EQ(input.GetMouseDelta().y, 5);
    event.type = InputEventType::Wheel;
    event.x = 1; event.y = 2;
    ASSERT_TRUE(input.SubmitEvent(event));
    ASSERT_TRUE(input.SubmitEvent(event));
    EXPECT_EQ(input.GetMouseWheel().x, 2);
    EXPECT_EQ(input.GetMouseWheel().y, 4);
}

TEST(InputSystemTests, AWindowHasOnlyOneIngressAndStaleHandlesAreRejected)
{
    InputSystem input;
    const auto window = input.RegisterWindow(123, IngressKind::Glfw, true);
    EXPECT_EQ(input.RegisterWindow(123, IngressKind::Glfw), window);
    EXPECT_FALSE(input.RegisterWindow(123, IngressKind::Win32));
    auto event = Button(input.GetKeyboard(), InputEventType::ButtonDown, 65, input.Now());
    event.window = window;
    event.ingress = IngressKind::Win32;
    EXPECT_FALSE(input.SubmitEvent(event));
    EXPECT_FALSE(input.WasButtonPressed(input.GetKeyboard(), 65));
    EXPECT_TRUE(input.UnregisterWindow(window));
    const auto replacement = input.RegisterWindow(456, IngressKind::Glfw, true);
    EXPECT_NE(replacement, window);
    event.ingress = IngressKind::Glfw;
    EXPECT_FALSE(input.SubmitEvent(event));
}

TEST(InputSystemTests, LosingOneWindowDoesNotResetAnotherFocusedWindow)
{
    InputSystem input;
    const auto first = input.RegisterWindow(1, IngressKind::Virtual, true);
    const auto second = input.RegisterWindow(2, IngressKind::Virtual, true);
    ASSERT_TRUE(input.SubmitEvent(Button(input.GetKeyboard(), InputEventType::ButtonDown, 65, input.Now())));
    InputEvent event;
    event.type = InputEventType::FocusLost;
    event.window = first;
    ASSERT_TRUE(input.SubmitEvent(event));
    EXPECT_TRUE(input.IsButtonDown(input.GetKeyboard(), 65));
    event.window = second;
    ASSERT_TRUE(input.SubmitEvent(event));
    input.FinishEventPump();
    EXPECT_FALSE(input.IsButtonDown(input.GetKeyboard(), 65));
    EXPECT_FALSE(input.WasButtonReleased(input.GetKeyboard(), 65));
}

TEST(InputSystemTests, DisconnectCancelsStateAndDoesNotAliasAReplacementDevice)
{
    InputSystem input;
    const auto pad = input.RegisterDevice({DeviceKind::Gamepad, "Test Pad", "Gamepad", "test", false});
    ASSERT_TRUE(input.SubmitEvent(Button(pad, InputEventType::ButtonDown, 0, input.Now())));
    ASSERT_TRUE(input.RemoveDevice(pad));
    EXPECT_FALSE(input.GetGamepadState(pad).connected);
    EXPECT_FALSE(input.WasButtonReleased(pad, 0));
    const auto replacement = input.RegisterDevice({DeviceKind::Gamepad, "Test Pad", "Gamepad", "test", false});
    EXPECT_NE(pad, replacement);
    EXPECT_FALSE(input.SubmitEvent(Button(pad, InputEventType::ButtonUp, 0, input.Now())));
}
