#include <gtest/gtest.h>
#include <Shortcuts/EditorShortcutService.h>
#include <Shortcuts/EditorShortcutAdapter.h>
#include <Input/InputSystem.h>
#include <Core/Inputs/InputSerialization.h>
#include <algorithm>
#include <fstream>
#include "EditorInputTestSupport.h"
using namespace NLS;
using namespace NLS::Editor::Shortcuts;
namespace
{
class EditorInputAdapterTests : public testing::Test
{
protected:
    double time = 0;
    Input::InputSystem input{[this] { return time; }};
    EditorShortcutService service;
    Tests::EditorInputMappingBuilder mappings;
    void Initialize() { service.InitializeInput(input); mappings.Apply(service); }
    void Register(std::string id, Windowing::Inputs::EKey key, int& calls, ShortcutContextId context = ShortcutContexts::Global,
        ShortcutModifiers modifiers = 0)
    {
        Tests::TestShortcutCommand command;
        command.id = id; command.context = context;
        command.defaultBinding = ShortcutBinding::FromKey(key, modifiers);
        command.execute = [&calls] { ++calls; };
        mappings.Add(command);
        if (!id.starts_with("scene-view.fly-")) ASSERT_TRUE(service.RegisterCommand(command));
    }
    void Key(uint16_t key, bool down, uint32_t modifiers = 0)
    {
        Input::InputEvent event;
        event.device = input.GetKeyboard(); event.control = key; event.modifiers = modifiers;
        event.type = down ? Input::InputEventType::ButtonDown : Input::InputEventType::ButtonUp;
        ASSERT_TRUE(input.SubmitEvent(event));
    }
    uint32_t PrimaryMask() const { return GetPrimaryModifierForCurrentPlatform() == EShortcutModifier::Super ? 8u : 2u; }
    const char* PrimaryName() const { return PrimaryMask() == 8u ? "Cmd" : "Ctrl"; }
    void Primary(bool down) { Key(PrimaryMask() == 8u ? 343 : 341, down, PrimaryMask()); }
    void LoadResourceActions()
    {
        service.InitializeInput(input);
        const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "App/Assets/Editor/Input/EditorShortcuts.inputactions";
        std::vector<std::string> diagnostics;
        ASSERT_TRUE(service.LoadDefaults(path, diagnostics));
    }
    void Mouse(Input::InputEventType type, uint16_t button = 0, double x = 0, double y = 0)
    {
        Input::InputEvent event; event.device = input.GetMouse(); event.type = type;
        event.control = button; event.x = x; event.y = y;
        ASSERT_TRUE(input.SubmitEvent(event));
    }
    void NextFrame() { service.PrepareInput({}); input.EndFrame(); time += .016; service.PrepareInput({}); }
};
}

TEST_F(EditorInputAdapterTests, ViewportAndCanvasOperationsHaveResourceSlotsAndNoEmptyCommands)
{
    LoadResourceActions();
    for (const auto& setting : EditorShortcutAdapter::ContinuousSettings())
    {
        EXPECT_FALSE(service.GetBindingSlots(setting.id).empty()) << setting.id;
        EXPECT_EQ(service.GetRegistry().FindCommand(setting.id), nullptr) << setting.id;
    }
    Mouse(Input::InputEventType::ButtonDown, 1); service.PrepareInput({});
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).pressed);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).actuated);
    NextFrame();
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).actuated);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraFly).pressed);
}

TEST_F(EditorInputAdapterTests, LateContinuousEventsRetainTheFirstPressAndDoNotRepeatDeltas)
{
    LoadResourceActions();
    Mouse(Input::InputEventType::ButtonDown, 1);
    Mouse(Input::InputEventType::PointerDelta, 0, 4, 3);
    EditorInputRoute text; text.textEditing = true; service.PrepareInput(text);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).pressed);
    auto look = service.GetInputState(EditorInputAction::CameraLook).value.AsVector2();
    EXPECT_FLOAT_EQ(look.x, 4); EXPECT_FLOAT_EQ(look.y, -3);
    Mouse(Input::InputEventType::ButtonUp, 1);
    Mouse(Input::InputEventType::PointerDelta, 0, 2, 1);
    const auto fly = service.GetInputState(EditorInputAction::CameraFly);
    EXPECT_TRUE(fly.pressed); EXPECT_TRUE(fly.released); EXPECT_FALSE(fly.actuated);
    for (int query = 0; query < 3; ++query)
    {
        look = service.GetInputState(EditorInputAction::CameraLook).value.AsVector2();
        EXPECT_FLOAT_EQ(look.x, 6); EXPECT_FLOAT_EQ(look.y, -4);
    }
    NextFrame();
    EXPECT_EQ(service.GetInputState(EditorInputAction::CameraLook).value.MagnitudeSquared(), 0);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraFly).pressed);
}

TEST_F(EditorInputAdapterTests, CameraPanAndOrbitSwitchWhileThePointerRemainsHeld)
{
    LoadResourceActions(); Mouse(Input::InputEventType::ButtonDown, 2); service.PrepareInput({});
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraPan).actuated);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraOrbit).actuated);
    NextFrame(); Key(342, true, 4);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraPan).actuated);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraOrbit).actuated);
    NextFrame(); Key(342, false);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraPan).actuated);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraOrbit).actuated);
    Mouse(Input::InputEventType::ButtonUp, 2);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraPan).actuated);
}

TEST_F(EditorInputAdapterTests, CanvasPanUsesTheResourceChordAndWheelModesRemainSeparate)
{
    LoadResourceActions(); Mouse(Input::InputEventType::ButtonDown, 0); service.PrepareInput({});
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CanvasPan).actuated);
    Mouse(Input::InputEventType::ButtonUp, 0); NextFrame(); Key(32, true);
    Mouse(Input::InputEventType::ButtonDown, 0);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CanvasPan).actuated);
    Mouse(Input::InputEventType::ButtonUp, 0); Key(32, false); NextFrame();
    Key(341, true, 2); Mouse(Input::InputEventType::Wheel, 0, 0, 2);
    EXPECT_FLOAT_EQ(service.GetInputState(EditorInputAction::CanvasZoom).value.vector.y, 2);
    EXPECT_EQ(service.GetInputState(EditorInputAction::CanvasScrollX).value.MagnitudeSquared(), 0);
    EXPECT_EQ(service.GetInputState(EditorInputAction::CanvasScrollY).value.MagnitudeSquared(), 0);
    Key(341, false); NextFrame(); Key(340, true, 1); Mouse(Input::InputEventType::Wheel, 0, 0, -1);
    EXPECT_FLOAT_EQ(service.GetInputState(EditorInputAction::CanvasScrollX).value.vector.y, -1);
    EXPECT_EQ(service.GetInputState(EditorInputAction::CanvasZoom).value.MagnitudeSquared(), 0);
}

TEST_F(EditorInputAdapterTests, NavigationRebindingReplacesThePhysicalButtonAndPersists)
{
    LoadResourceActions(); service.PrepareInput({});
    const auto slot = service.GetBindingSlots("camera-navigation.fly").front();
    std::string error;
    ASSERT_TRUE(service.SetBindingSlot("camera-navigation.fly", 0, {slot, Input::OverrideState::Override,
        {{{Input::DeviceKind::Mouse, Input::ControlKind::Button, 3}, {1}}}}, error)) << error;
    NextFrame(); Mouse(Input::InputEventType::ButtonDown, 1);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraFly).actuated);
    Mouse(Input::InputEventType::ButtonUp, 1); Mouse(Input::InputEventType::ButtonDown, 3);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).actuated);
    const auto path = std::filesystem::temp_directory_path() / ("nullus-camera-profile-" + Guid::New().ToString() + ".inputprofile");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
    ASSERT_TRUE(service.SaveProfile(path));
    EditorShortcutService other; other.InitializeInput(input);
    const auto defaults = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "App/Assets/Editor/Input/EditorShortcuts.inputactions";
    std::vector<std::string> diagnostics; ASSERT_TRUE(other.LoadDefaults(defaults, diagnostics));
    ASSERT_TRUE(other.LoadProfile(path));
    EXPECT_EQ(other.GetInputUser()->GetEffectiveBinding(slot).front().path.control, 3);
}

TEST_F(EditorInputAdapterTests, GameOwnershipCancelsContinuousEditorInputs)
{
    LoadResourceActions(); Mouse(Input::InputEventType::ButtonDown, 1); service.PrepareInput({});
    ASSERT_TRUE(service.GetInputState(EditorInputAction::CameraFly).actuated);
    Mouse(Input::InputEventType::PointerDelta, 0, .1, .1);
    Mouse(Input::InputEventType::Wheel, 0, 0, .1);
    EXPECT_GT(service.GetInputState(EditorInputAction::CameraZoom).value.MagnitudeSquared(), 0);
    EditorInputRoute game; game.gameOwnsInput = true; service.PrepareInput(game);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraFly).actuated);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraFly).pressed);
    EXPECT_EQ(service.GetInputState(EditorInputAction::CameraLook).value.MagnitudeSquared(), 0);
    EXPECT_EQ(service.GetInputState(EditorInputAction::CameraZoom).value.MagnitudeSquared(), 0);
    input.EndFrame(); time += .016; service.PrepareInput({});
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraFly).pressed);
}

TEST_F(EditorInputAdapterTests, ContinuousModifiersDoNotConsumeTheSaveCommand)
{
    int saves = 0;
    ShortcutCommand command; command.id = "file.save-scene"; command.context = ShortcutContexts::Global;
    command.execute = [&] { ++saves; }; ASSERT_TRUE(service.RegisterCommand(std::move(command)));
    LoadResourceActions(); Key(341, true, 2); Key(83, true, 2); Key(83, false, 2);
    service.UpdateInput({});
    EXPECT_TRUE(service.GetInputState(EditorInputAction::SceneSnap).actuated);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::SceneToggleSelection).actuated);
    EXPECT_EQ(saves, 1);
}

TEST_F(EditorInputAdapterTests, ContinuousHandlesRefreshSafelyWhenHeldDefaultsReload)
{
    LoadResourceActions(); Mouse(Input::InputEventType::ButtonDown, 1); service.PrepareInput({});
    ASSERT_TRUE(service.GetInputState(EditorInputAction::CameraFly).actuated);
    const auto slot = service.GetBindingSlots("camera-navigation.fly").front();
    const auto defaults = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "App/Assets/Editor/Input/EditorShortcuts.inputactions";
    std::vector<std::string> diagnostics; ASSERT_TRUE(service.LoadDefaults(defaults, diagnostics));
    service.PrepareInput({});
    EXPECT_EQ(service.GetBindingSlots("camera-navigation.fly").front(), slot);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraFly).pressed);
    Mouse(Input::InputEventType::ButtonUp, 1); service.PrepareInput({}); NextFrame();
    Mouse(Input::InputEventType::ButtonDown, 1);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).pressed);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).actuated);
}

TEST_F(EditorInputAdapterTests, SceneGestureCommandSharesItsActionAndExecutesOnlyOnce)
{
    LoadResourceActions(); int canceled = 0;
    ShortcutCommand command; command.id = "scene-view.cancel-gesture"; command.context = "scene-input-gesture";
    command.execute = [&] { ++canceled; }; ASSERT_TRUE(service.RegisterCommand(std::move(command)));
    service.RegisterContext({"scene-input-gesture", "Scene Gesture", 160, "", [] { return true; }});
    Key(256, true); Key(256, false); service.PrepareInput({});
    ASSERT_EQ(service.GetBindingSlots("scene-view.cancel-gesture").size(), 1u);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::SceneCancel).pressed);
    service.DispatchInput({}); EXPECT_EQ(canceled, 1);
    service.DispatchInput({}); EXPECT_EQ(canceled, 1);
    const auto settings = service.GetCommands();
    EXPECT_EQ(std::count_if(settings.begin(), settings.end(), [](const auto* value) { return value->id == "scene-view.cancel-gesture"; }), 1);
}

TEST_F(EditorInputAdapterTests, TimedNavigationBindingCanActuateWithoutAnotherDeviceEvent)
{
    LoadResourceActions();
    auto definition = service.GetInputUser()->GetDefinition()->GetDefinition();
    const auto id = EditorShortcutAdapter::ActionForCommand("camera-navigation.fly");
    for (auto& context : definition.contexts)
        for (auto& binding : context.bindings)
            if (binding.action == id) { binding.hasInteraction = true; binding.interaction = {Input::InteractionKind::Hold, .1}; }
    std::vector<std::string> diagnostics;
    auto compiled = Input::CompiledInput::Compile(std::move(definition), diagnostics);
    ASSERT_TRUE(compiled); ASSERT_TRUE(service.SetInputDefaults(std::move(compiled)));
    Mouse(Input::InputEventType::ButtonDown, 1); service.PrepareInput({});
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraFly).actuated);
    input.EndFrame(); time = .15; service.PrepareInput({});
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).pressed);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).actuated);
}

TEST_F(EditorInputAdapterTests, QueriesBeforePreparationDoNotSwallowTheFirstPressAfterGameOwnership)
{
    LoadResourceActions(); EditorInputRoute game; game.gameOwnsInput = true; service.PrepareInput(game);
    input.EndFrame(); time += .016;
    Mouse(Input::InputEventType::ButtonDown, 1);
    EXPECT_FALSE(service.GetInputState(EditorInputAction::CameraFly).pressed);
    service.PrepareInput({});
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).pressed);
    EXPECT_TRUE(service.GetInputState(EditorInputAction::CameraFly).actuated);
}
TEST_F(EditorInputAdapterTests, TwoPhysicalPressesInAFrameExecuteTwiceThroughOnePath)
{
    int calls = 0; Register("file.save-scene", Windowing::Inputs::EKey::KEY_S, calls, ShortcutContexts::Global, ToModifierMask(EShortcutModifier::Ctrl));
    Initialize();
    Key(341, true, 2);
    Key(83, true, 2); Key(83, false, 2); Key(83, true, 2); Key(83, false, 2);
    service.UpdateInput({});
    EXPECT_EQ(calls, 2);
    service.UpdateInput({}); EXPECT_EQ(calls, 2);
}
TEST_F(EditorInputAdapterTests, ReleasesPumpedInsideSaveCommandDoNotSwallowTheNextSave)
{
    int calls = 0;
    Register("file.save-scene", Windowing::Inputs::EKey::KEY_S, calls,
        ShortcutContexts::Global, ToModifierMask(EShortcutModifier::Primary));
    service.GetRegistry().FindCommand("file.save-scene")->execute = [&]
    {
        ++calls;
        // A native Save As dialog runs a nested event pump after Action
        // evaluation. Its releases must reach the next input update.
        Key(83, false, PrimaryMask()); Primary(false);
    };
    Initialize();
    for (int press = 1; press <= 3; ++press)
    {
        Primary(true); Key(83, true, PrimaryMask()); input.FinishEventPump();
        service.UpdateInput({});
        EXPECT_EQ(calls, press);
        input.EndFrame(); time += .016;
    }
}
TEST_F(EditorInputAdapterTests, RealDefaultSaveChordWorksOnTheFirstPressAcrossFrames)
{
    int calls = 0;
    Register("file.save-scene", Windowing::Inputs::EKey::KEY_S, calls);
    Initialize();
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "App/Assets/Editor/Input/EditorShortcuts.inputactions";
    std::vector<std::string> diagnostics; ASSERT_TRUE(service.LoadDefaults(path, diagnostics));
    for (int press = 1; press <= 3; ++press)
    {
        // The shipped Save binding explicitly uses Control, including on macOS.
        Key(341, true, 2); input.FinishEventPump(); service.UpdateInput({}); input.EndFrame(); time += .016;
        Key(83, true, 2); input.FinishEventPump(); service.UpdateInput({});
        EXPECT_EQ(calls, press);
        input.EndFrame(); time += .016;
        Key(83, false, 2); Key(341, false); input.FinishEventPump(); service.UpdateInput({});
        EXPECT_EQ(calls, press);
        input.EndFrame(); time += .016;
    }
}
TEST_F(EditorInputAdapterTests, SceneAndHierarchyReuseDeleteWithoutDoubleExecution)
{
    int scene = 0, hierarchy = 0; bool sceneFocused = true;
    Register("scene.delete", Windowing::Inputs::EKey::KEY_DELETE, scene, ShortcutContexts::SceneView);
    Register("hierarchy.delete", Windowing::Inputs::EKey::KEY_DELETE, hierarchy, ShortcutContexts::Hierarchy);
    service.RegisterContext({ShortcutContexts::SceneView, "Scene", 20, "panel", [&] { return sceneFocused; }});
    service.RegisterContext({ShortcutContexts::Hierarchy, "Hierarchy", 20, "panel", [&] { return !sceneFocused; }});
    Initialize();
    auto renamed = service.GetInputUser()->GetDefinition()->GetDefinition();
    for (auto& context : renamed.contexts) context.name = "Display: " + context.name;
    std::vector<std::string> diagnostics;
    auto compiled = Input::CompiledInput::Compile(std::move(renamed), diagnostics); ASSERT_TRUE(compiled);
    service.GetInputUser()->SetDefinition(compiled);
    EXPECT_TRUE(service.ValidateBinding("scene.delete", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_DELETE)).empty());
    EXPECT_TRUE(service.SetBinding("scene.delete", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_DELETE)));
    Key(261, true); Key(261, false); service.UpdateInput({});
    EXPECT_EQ(scene, 1); EXPECT_EQ(hierarchy, 0);
    sceneFocused = false; input.EndFrame();
    Key(261, true); Key(261, false); service.UpdateInput({});
    EXPECT_EQ(scene, 1); EXPECT_EQ(hierarchy, 1);
}
TEST_F(EditorInputAdapterTests, CurrentOwnerAuthorizesDeferredCallbacksAndNavigationIsReadyBeforeDispatch)
{
    int calls = 0; bool focused = true;
    Register("document.save", Windowing::Inputs::EKey::KEY_S, calls, "document", ToModifierMask(EShortcutModifier::Primary));
    service.RegisterContext({"document", "Document", 50, "panel", [&] { return focused; }});
    Initialize();
    EditorInputRoute route; route.workbenchOwnsInput = true; route.ownedScope = "document";
    Primary(true); Key(83, true, PrimaryMask()); Key(83, false, PrimaryMask());
    service.PrepareInput(route); EXPECT_EQ(calls, 0);
    focused = false;
    service.DispatchInput(route); EXPECT_EQ(calls, 0);
    input.EndFrame(); focused = true;
    Key(83, true, PrimaryMask()); Key(83, false, PrimaryMask());
    service.PrepareInput(route); service.DispatchInput(route); EXPECT_EQ(calls, 1);
}
TEST_F(EditorInputAdapterTests, FocusAcquiredDuringDrawingKeepsEveryPendingShortPress)
{
    int calls=0, global=0; bool focused=false;
    Register("document.save",Windowing::Inputs::EKey::KEY_S,calls,"document",ToModifierMask(EShortcutModifier::Ctrl));
    Register("file.save-scene",Windowing::Inputs::EKey::KEY_S,global,ShortcutContexts::Global,ToModifierMask(EShortcutModifier::Ctrl));
    service.RegisterContext({"document","Document",50,"panel",[&]{return focused;}});Initialize();
    Key(341,true,2); for(int i=0;i<2;++i){Key(83,true,2);Key(83,false,2);}
    service.PrepareInput({}); focused=true;
    EditorInputRoute route;route.workbenchOwnsInput=true;route.ownedScope="document";
    service.DispatchInput(route);EXPECT_EQ(calls,2);EXPECT_EQ(global,0);
}
TEST_F(EditorInputAdapterTests, RegisteringAnUnrelatedCommandDuringDispatchDoesNotDropLaterInput)
{
    int first=0, second=0, dynamic=0;
    Register("first",Windowing::Inputs::EKey::KEY_A,first);
    Register("second",Windowing::Inputs::EKey::KEY_B,second);
    service.GetRegistry().FindCommand("first")->execute=[&]{ ++first; Register("dynamic",Windowing::Inputs::EKey::KEY_C,dynamic); };
    Initialize();
    Key(65,true);Key(65,false);Key(66,true);Key(66,false);service.UpdateInput({});
    EXPECT_EQ(first,1);EXPECT_EQ(second,1);EXPECT_EQ(dynamic,0);
}
TEST_F(EditorInputAdapterTests, TextModalAndGameOwnershipBlockOrdinaryCommands)
{
    int calls = 0; Register("scene.move", Windowing::Inputs::EKey::KEY_W, calls);
    Initialize();
    for (int route = 0; route < 3; ++route)
    {
        EditorInputRoute state; state.textEditing = route == 0; state.modal = route == 1; state.gameOwnsInput = route == 2;
        Key(87, true); Key(87, false); service.UpdateInput(state); input.EndFrame();
    }
    EXPECT_EQ(calls, 0);
    Key(87, true); Key(87, false); service.UpdateInput({}); EXPECT_EQ(calls, 1);
}
TEST_F(EditorInputAdapterTests, DisabledHighScopeDoesNotFallBackToGlobalCommand)
{
    int high = 0, global = 0;
    Register("global.delete", Windowing::Inputs::EKey::KEY_DELETE, global);
    Register("scene.delete", Windowing::Inputs::EKey::KEY_DELETE, high, ShortcutContexts::SceneView);
    service.RegisterContext({ShortcutContexts::SceneView, "Scene", 20, "panel", [] { return true; }});
    service.GetRegistry().FindCommand("scene.delete")->availability = [] { return false; };
    Initialize();
    Key(261, true); Key(261, false); service.UpdateInput({});
    EXPECT_EQ(high, 0); EXPECT_EQ(global, 0);
}
TEST_F(EditorInputAdapterTests, MenuAndShortcutShareCommandAvailabilityAndExecution)
{
    int calls = 0; Register("file.save-scene", Windowing::Inputs::EKey::KEY_S, calls);
    Initialize();
    InvocationContext menu; menu.source = CommandSource::Menu;
    EXPECT_EQ(service.TryExecuteCommand("file.save-scene", menu), CommandResult::Completed);
    Key(83, true); Key(83, false); service.UpdateInput({});
    EXPECT_EQ(calls, 2);
    service.GetRegistry().FindCommand("file.save-scene")->availability = [] { return false; };
    EXPECT_EQ(service.TryExecuteCommand("file.save-scene", menu), CommandResult::Disabled);
    EXPECT_EQ(calls, 2);
}

TEST_F(EditorInputAdapterTests, DefaultUnboundAndOverrideSnapshotsRestoreAllAliases)
{
    int calls = 0;
    Register("edit.redo", Windowing::Inputs::EKey::KEY_Y, calls, ShortcutContexts::Global, ToModifierMask(EShortcutModifier::Primary));
    mappings.AddAlias("edit.redo", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_Z, EShortcutModifier::Primary | EShortcutModifier::Shift));
    Initialize();
    const auto defaults = service.CaptureBindingState("edit.redo");
    EXPECT_TRUE(service.SetBinding("edit.redo", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_R)));
    const auto overridden = service.CaptureBindingState("edit.redo");
    EXPECT_NE(overridden, defaults);
    EXPECT_EQ(service.GetBindingDisplayText("edit.redo"), "R");
    EXPECT_TRUE(service.ClearBinding("edit.redo"));
    const auto unbound = service.CaptureBindingState("edit.redo");
    EXPECT_NE(unbound, defaults); EXPECT_NE(unbound, overridden);
    EXPECT_TRUE(service.RestoreBindingState("edit.redo", defaults));
    EXPECT_EQ(service.GetBindingDisplayText("edit.redo"), std::string(PrimaryName()) + " + Y / " + PrimaryName() + " + Shift + Z");
    EXPECT_TRUE(service.RestoreBindingState("edit.redo", unbound));
    EXPECT_EQ(service.GetBindingDisplayText("edit.redo"), "Unassigned");
    EXPECT_TRUE(service.ResetBinding("edit.redo"));
    EXPECT_EQ(service.CaptureBindingState("edit.redo"), defaults);
}

TEST_F(EditorInputAdapterTests, NavigationSubsetDoesNotConsumeUnmatchedGlobalSaveChord)
{
    int navigation = 0, save = 0;
    Register("scene-view.fly-backward", Windowing::Inputs::EKey::KEY_S, navigation, ShortcutContexts::SceneViewFlyMode);
    Register("file.save-scene", Windowing::Inputs::EKey::KEY_S, save, ShortcutContexts::Global, ToModifierMask(EShortcutModifier::Ctrl));
    service.RegisterContext({ShortcutContexts::SceneViewFlyMode, "Fly", 40, "", [] { return true; }});
    Initialize();
    Key(341, true, 2); Key(83, true, 2); Key(83, false, 2); service.UpdateInput({});
    EXPECT_EQ(save, 1); EXPECT_EQ(navigation, 0);
}

TEST_F(EditorInputAdapterTests, LegacyOverrideSuppressesAliasesAndSavesOnlyTheNewProfile)
{
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "build/input-profile-tests" / Guid::New().ToString();
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    const std::string legacy = R"({"version":1,"bindings":{"edit.redo":{"assigned":true,"key":"82","modifiers":16},"removed.command":{"assigned":true,"key":"88","modifiers":0}},"unassigned":[]})";
    { std::ofstream out(root / "shortcuts.json"); out << legacy; }
    int calls = 0; Register("edit.redo", Windowing::Inputs::EKey::KEY_Y, calls, ShortcutContexts::Global, ToModifierMask(EShortcutModifier::Primary));
    mappings.AddAlias("edit.redo", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_Z, EShortcutModifier::Primary | EShortcutModifier::Shift));
    Initialize(); ASSERT_TRUE(service.LoadProfile(root / "editor.inputprofile", root / "shortcuts.json"));
    EXPECT_EQ(service.GetBindingDisplayText("edit.redo"), std::string(PrimaryName()) + " + R");
    EXPECT_TRUE(service.GetInputUser()->GetEffectiveBinding(EditorShortcutAdapter::BindingForCommand("edit.redo", 1)).empty());
    EXPECT_FALSE(service.GetInputUser()->GetUnappliedOverrides().empty());
    ASSERT_TRUE(service.SaveProfile(root / "editor.inputprofile"));
    Input::InputProfile loaded; std::vector<std::string> diagnostics;
    ASSERT_TRUE(NLS::Core::Inputs::LoadInputProfile(root / "editor.inputprofile", loaded, diagnostics));
    EXPECT_FALSE(loaded.overrides.empty());
    std::ifstream old(root / "shortcuts.json"); EXPECT_EQ(std::string(std::istreambuf_iterator<char>(old), {}), legacy);
}
TEST_F(EditorInputAdapterTests, RestoringDefaultRejectsAConflictWithoutChangingTheOverride)
{
    int first = 0, second = 0; Register("first", Windowing::Inputs::EKey::KEY_S, first); Register("second", Windowing::Inputs::EKey::KEY_O, second);
    Initialize();
    ASSERT_TRUE(service.SetBinding("first", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_X)));
    ASSERT_TRUE(service.SetBinding("second", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_S)));
    std::string error; EXPECT_FALSE(service.SetBindingSlot("first", 0, {EditorShortcutAdapter::BindingForCommand("first", 0), Input::OverrideState::Default}, error));
    EXPECT_FALSE(error.empty()); EXPECT_EQ(service.GetBindingDisplayText("first"), "X");
}
TEST_F(EditorInputAdapterTests, RestoringGlobalSaveDefaultAllowsItsOwnedDocumentCounterpart)
{
    int global = 0, document = 0;
    const auto primary = ToModifierMask(EShortcutModifier::Primary);
    Register("file.save-scene", Windowing::Inputs::EKey::KEY_S, global, ShortcutContexts::Global, primary);
    Register("ui-designer.save", Windowing::Inputs::EKey::KEY_S, document, "ui-designer", primary);
    service.RegisterContext({"ui-designer", "UI Designer", 50, "panel", [] { return true; }}); Initialize();
    ASSERT_TRUE(service.SetBinding("file.save-scene", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_O, primary)));
    std::string error;
    EXPECT_TRUE(service.SetBindingSlot("file.save-scene", 0,
        {EditorShortcutAdapter::BindingForCommand("file.save-scene", 0), Input::OverrideState::Default}, error)) << error;
    Primary(true); Key(83, true, PrimaryMask()); Key(83, false, PrimaryMask());
    EditorInputRoute route; route.workbenchOwnsInput = true; route.ownedScope = "ui-designer";
    service.UpdateInput(route); EXPECT_EQ(global, 0); EXPECT_EQ(document, 1);
}

TEST_F(EditorInputAdapterTests, ExplicitTextWhitelistDoesNotMistakeMouseControlForPrintableKey)
{
    int calls = 0; Register("editor.stop", Windowing::Inputs::EKey::KEY_F8, calls); Initialize();
    const auto binding = EditorShortcutAdapter::BindingForCommand("editor.stop", 0);
    service.GetInputUser()->SetBindingOverride({binding, Input::OverrideState::Override,
        {{{Input::DeviceKind::Mouse, Input::ControlKind::Button, 3}, {1}}}});
    Input::InputEvent event; event.device = input.GetMouse(); event.control = 3; event.type = Input::InputEventType::ButtonDown;
    ASSERT_TRUE(input.SubmitEvent(event)); event.type = Input::InputEventType::ButtonUp; ASSERT_TRUE(input.SubmitEvent(event));
    EditorInputRoute route; route.textEditing = true; service.UpdateInput(route); EXPECT_EQ(calls, 1); input.EndFrame();
    ASSERT_TRUE(service.SetBinding("editor.stop", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_W)));
    Key(87, true); Key(87, false); service.UpdateInput(route); EXPECT_EQ(calls, 1);
}

TEST_F(EditorInputAdapterTests, NativeWidgetActivationCannotAlsoExecuteItsMouseOrKeyboardShortcut)
{
    int calls = 0; Register("custom.save", Windowing::Inputs::EKey::KEY_F8, calls); Initialize();
    const auto binding = EditorShortcutAdapter::BindingForCommand("custom.save", 0);
    service.GetInputUser()->SetBindingOverride({binding, Input::OverrideState::Override,
        {{{Input::DeviceKind::Mouse, Input::ControlKind::Button, 0}, {1}}}});
    InvocationContext native; native.source = CommandSource::Toolbar;
    EXPECT_EQ(service.TryExecuteCommand("custom.save", native), CommandResult::Completed);
    Input::InputEvent event; event.device = input.GetMouse(); event.type = Input::InputEventType::ButtonDown;
    ASSERT_TRUE(input.SubmitEvent(event)); event.type = Input::InputEventType::ButtonUp; ASSERT_TRUE(input.SubmitEvent(event));
    EditorInputRoute route; route.widgetOwnsPointer = true; service.UpdateInput(route); EXPECT_EQ(calls, 1); input.EndFrame();
    ASSERT_TRUE(service.SetBinding("custom.save", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_ENTER)));
    EXPECT_EQ(service.TryExecuteCommand("custom.save", native), CommandResult::Completed);
    Key(257, true); Key(257, false); route.widgetKeyboardActivation = true;
    service.UpdateInput(route); EXPECT_EQ(calls, 2); input.EndFrame();
    Key(257, true); Key(257, false); service.UpdateInput({}); EXPECT_EQ(calls, 3);
}

TEST_F(EditorInputAdapterTests, EditorCanRebindToMouseAndGamepadWithoutAnotherInputBackend)
{
    int calls = 0; Register("custom.command", Windowing::Inputs::EKey::KEY_F8, calls); Initialize();
    const auto pad = input.RegisterDevice({Input::DeviceKind::Gamepad, "Virtual Pad", "Gamepad"}); service.UpdateInput({}); input.EndFrame();
    auto session = service.BeginRebinding("custom.command", 0); ASSERT_TRUE(session);
    Input::InputEvent event; event.device = pad; event.control = 0; event.type = Input::InputEventType::ButtonDown; ASSERT_TRUE(input.SubmitEvent(event));
    ASSERT_EQ(session->Update(), Input::RebindingStatus::Completed);
    std::string error; ASSERT_TRUE(service.SetBindingSlot("custom.command", 0, session->GetResult(), error)) << error; session.reset();
    event.type = Input::InputEventType::ButtonUp; ASSERT_TRUE(input.SubmitEvent(event)); service.UpdateInput({}); input.EndFrame();
    event.type = Input::InputEventType::ButtonDown; ASSERT_TRUE(input.SubmitEvent(event)); event.type = Input::InputEventType::ButtonUp; ASSERT_TRUE(input.SubmitEvent(event)); service.UpdateInput({}); EXPECT_EQ(calls, 1);
    input.EndFrame(); session = service.BeginRebinding("custom.command", 0); event.device = input.GetMouse(); event.control = 3; event.type = Input::InputEventType::ButtonDown; ASSERT_TRUE(input.SubmitEvent(event));
    ASSERT_EQ(session->Update(), Input::RebindingStatus::Completed); ASSERT_TRUE(service.SetBindingSlot("custom.command", 0, session->GetResult(), error)); session.reset();
    event.type = Input::InputEventType::ButtonUp; ASSERT_TRUE(input.SubmitEvent(event)); service.UpdateInput({}); input.EndFrame();
    event.type = Input::InputEventType::ButtonDown; ASSERT_TRUE(input.SubmitEvent(event)); event.type = Input::InputEventType::ButtonUp; ASSERT_TRUE(input.SubmitEvent(event)); service.UpdateInput({}); EXPECT_EQ(calls, 2);
}

TEST_F(EditorInputAdapterTests, AssetOwnsDefaultsAndNavigationHasNoBusinessCommand)
{
    int calls = 0; Register("file.save-scene", Windowing::Inputs::EKey::KEY_F8, calls);
    service.RegisterContext({ShortcutContexts::SceneViewFlyMode, "Fly", 40, "", [] { return true; }});
    Initialize();
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "App/Assets/Editor/Input/EditorShortcuts.inputactions";
    std::vector<std::string> diagnostics; ASSERT_TRUE(service.LoadDefaults(path, diagnostics));
    EXPECT_EQ(service.GetBindingDisplayText("file.save-scene"), "Ctrl + S");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.fly-forward"), "W");
    EXPECT_EQ(service.GetRegistry().FindCommand("scene-view.fly-forward"), nullptr);
    EXPECT_EQ(service.TryExecuteCommand("scene-view.fly-forward"), CommandResult::UnknownCommand);
    Key(87, true); service.PrepareInput({});
    EXPECT_FLOAT_EQ(service.GetNavigationValue().z, 1);
    service.DispatchInput({}); EXPECT_EQ(calls, 0); input.EndFrame();
    Key(87, false); Key(341, true, 2); Key(83, true, 2); Key(83, false, 2);
    service.UpdateInput({}); EXPECT_EQ(calls, 1);
}

TEST_F(EditorInputAdapterTests, SlotEditsResolveAssetBindingIdsAfterReordering)
{
    int calls = 0; Register("custom.command", Windowing::Inputs::EKey::KEY_F8, calls);
    mappings.AddAlias("custom.command", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_F9));
    Initialize();
    const auto primary = service.GetBindingSlots("custom.command")[0];
    const auto alternate = service.GetBindingSlots("custom.command")[1];
    std::string error;
    ASSERT_TRUE(service.SetBindingSlot("custom.command", 0, {primary, Input::OverrideState::Override,
        {{{Input::DeviceKind::Keyboard, Input::ControlKind::Button, 88}, {1}}}}, error));
    auto definition = mappings.definition;
    for (auto& context : definition.contexts) std::reverse(context.bindings.begin(), context.bindings.end());
    std::vector<std::string> diagnostics;
    ASSERT_TRUE(service.SetInputDefaults(Input::CompiledInput::Compile(std::move(definition), diagnostics)));
    ASSERT_EQ(service.GetBindingSlots("custom.command")[0], alternate);
    ASSERT_TRUE(service.SetBindingSlot("custom.command", 0, {alternate, Input::OverrideState::ExplicitlyUnbound}, error));
    EXPECT_EQ(service.GetBindingDisplayText("custom.command"), "X");
    EXPECT_EQ(service.GetInputUser()->GetEffectiveBinding(primary)[0].path.control, 88);
    Key(298, true); Key(298, false); service.UpdateInput({}); EXPECT_EQ(calls, 0); input.EndFrame();
    Key(88, true); Key(88, false); service.UpdateInput({}); EXPECT_EQ(calls, 1);
}

TEST_F(EditorInputAdapterTests, AssetReloadCancelsHeldActionAndRetainsActiveSubscriptions)
{
    int calls = 0; Register("custom.command", Windowing::Inputs::EKey::KEY_F8, calls); Initialize();
    int performed = 0, canceled = 0;
    const auto action = EditorShortcutAdapter::ActionForCommand("custom.command");
    auto onPerformed = service.GetInputUser()->BindAction(action, Input::ActionPhase::Performed, [&](const auto&) { ++performed; });
    auto onCanceled = service.GetInputUser()->BindAction(action, Input::ActionPhase::Canceled, [&](const auto&) { ++canceled; });
    Key(297, true); service.UpdateInput({}); EXPECT_EQ(calls, 1); input.EndFrame();
    auto definition = mappings.definition;
    definition.contexts.front().bindings.front().parts[0].path.control = 298;
    std::vector<std::string> diagnostics;
    ASSERT_TRUE(service.SetInputDefaults(Input::CompiledInput::Compile(std::move(definition), diagnostics)));
    Key(297, false); Key(298, true); Key(298, false); service.UpdateInput({});
    EXPECT_EQ(canceled, 1); EXPECT_EQ(performed, 2); EXPECT_EQ(calls, 2);
}

TEST_F(EditorInputAdapterTests, CommandResetValidatesEveryAliasBeforeCommitting)
{
    int first = 0, second = 0;
    Register("first", Windowing::Inputs::EKey::KEY_F8, first);
    mappings.AddAlias("first", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_F9));
    Register("second", Windowing::Inputs::EKey::KEY_F10, second); Initialize();
    ASSERT_TRUE(service.SetBinding("first", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_F11)));
    ASSERT_TRUE(service.SetBinding("second", ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_F9)));
    const auto before = service.CaptureBindingState("first");
    EXPECT_FALSE(service.ResetBinding("first"));
    EXPECT_EQ(service.CaptureBindingState("first"), before);
}
