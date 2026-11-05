#include <gtest/gtest.h>
#include <Input/InputDomain.h>
#include <Input/InputRebinding.h>
#include <Input/InputReplay.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>

using namespace NLS;
using namespace NLS::Input;

TEST(InputRebindingTests, CompositeDirectionAndOtherPartsSurviveRebinding)
{
    InputSystem system;
    InputUser user(system, 1);
    InputDefinition definition; definition.id = Guid::New();
    ActionDefinition action; action.id = Guid::New(); action.type = ValueType::Vector2; definition.actions.push_back(action);
    ContextDefinition context; context.id = Guid::New();
    BindingDefinition binding; binding.id = Guid::New(); binding.action = action.id; binding.composite = Composite::Vector2;
    binding.parts = {{{DeviceKind::Keyboard, ControlKind::Button, 65}, {-1,0}}, {{DeviceKind::Keyboard, ControlKind::Button, 68}, {1,0}}};
    context.bindings.push_back(binding); definition.contexts.push_back(context);
    std::vector<std::string> diagnostics; user.SetDefinition(CompiledInput::Compile(definition, diagnostics));
    RebindingOptions options; options.partIndex = 0;
    RebindingSession rebind(system, user, binding.id, options);
    InputEvent event; event.type = InputEventType::ButtonDown; event.control = 74; event.device = system.GetKeyboard();
    ASSERT_TRUE(system.SubmitEvent(event)); ASSERT_EQ(rebind.Update(), RebindingStatus::Completed); ASSERT_TRUE(rebind.Apply());
    const auto effective = user.GetEffectiveBinding(binding.id);
    ASSERT_EQ(effective.size(), 2u); EXPECT_EQ(effective[0].path.control, 74); EXPECT_EQ(effective[0].contribution, InputValue(-1,0));
    EXPECT_EQ(effective[1].path.control, 68); EXPECT_EQ(effective[1].contribution, InputValue(1,0));
}
TEST(InputRebindingTests, UnboundLogicalSlotRetainsItsIdentityAndDirectionWhenRebound)
{
    InputSystem system; InputUser user(system, 1); InputDefinition definition; definition.id = Guid::New();
    ActionDefinition action; action.id = Guid::New(); action.type = ValueType::Vector2; definition.actions.push_back(action);
    ContextDefinition context; context.id = Guid::New(); BindingDefinition binding; binding.id = Guid::New(); binding.action = action.id; binding.composite = Composite::Vector2;
    binding.parts = {{{DeviceKind::Keyboard, ControlKind::LogicalKey, 0, {}, 65}, {0, -1}}}; context.bindings.push_back(binding); definition.contexts.push_back(context);
    std::vector<std::string> diagnostics; user.SetDefinition(CompiledInput::Compile(definition, diagnostics)); user.ClearBinding(binding.id);
    RebindingSession session(system, user, binding.id);
    InputEvent event; event.type = InputEventType::ButtonDown; event.device = system.GetKeyboard(); event.control = 66; event.scanCode = 48; event.logicalKey = 0x4e2d;
    ASSERT_TRUE(system.SubmitEvent(event)); ASSERT_EQ(session.Update(), RebindingStatus::Completed); ASSERT_TRUE(session.Apply());
    const auto parts = user.GetEffectiveBinding(binding.id); ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0].path.kind, ControlKind::LogicalKey); EXPECT_EQ(parts[0].path.code, 0x4e2du); EXPECT_EQ(parts[0].contribution, InputValue(0, -1));
}
TEST(InputProfileTests, ReloadRetainsButDoesNotEvaluateIncompatibleOrUnresolvedOverrides)
{
    InputSystem system; InputUser user(system,1); InputDefinition definition; definition.id=Guid::New();
    ActionDefinition action; action.id=Guid::New(); definition.actions.push_back(action);
    ContextDefinition context; context.id=Guid::New(); BindingDefinition binding; binding.id=Guid::New(); binding.action=action.id;
    binding.parts={{{DeviceKind::Keyboard,ControlKind::Button,32},{1}}};context.bindings.push_back(binding);definition.contexts.push_back(context);
    std::vector<std::string> diagnostics;user.SetDefinition(CompiledInput::Compile(definition,diagnostics));
    InputProfile profile; profile.overrides.push_back({binding.id,OverrideState::Override,{{{DeviceKind::Mouse,ControlKind::PointerDelta,0},{1}}}});
    auto unknown=Guid::New();profile.unresolved.push_back({unknown,OverrideState::ExplicitlyUnbound});user.SetProfile(profile);
    EXPECT_EQ(user.GetEffectiveBinding(binding.id)[0].path.control,32);
    EXPECT_EQ(user.GetUnappliedOverrides().size(),2u); EXPECT_EQ(user.GetProfile().overrides.size(),1u);
    user.RestoreBindingDefault(binding.id);EXPECT_EQ(user.GetUnappliedOverrides().size(),1u);
}
TEST(InputLifecycleTests, HostUserIdsAreNotReusedAcrossPlayDomains)
{
    InputSystem system; uint64_t previous=0;
    { InputDomain first(system,"Play"); previous=first.CreateUser()->GetId(); }
    InputDomain next(system,"Next Play"); EXPECT_NE(next.CreateUser()->GetId(),previous);
}
TEST(InputPerformanceTests, SteadyActiveActionsKeepTheFixedBufferEmptyForEditorUsers)
{
    double time = 0; InputSystem system([&] { return time; }); InputUser user(system, 1);
    InputDefinition definition; definition.id = Guid::New(); ContextDefinition context; context.id = Guid::New();
    for (int i = 0; i < 128; ++i)
    {
        ActionDefinition action; action.id = Guid::New(); definition.actions.push_back(action);
        BindingDefinition binding; binding.id = Guid::New(); binding.action = action.id;
        binding.parts = {{{DeviceKind::Keyboard, ControlKind::Button, 32}, {1}}}; context.bindings.push_back(binding);
    }
    definition.contexts.push_back(context); std::vector<std::string> diagnostics;
    user.SetDefinition(CompiledInput::Compile(definition, diagnostics)); user.PushContext(context.id); user.SetFixedBufferEnabled(false);
    InputEvent event; event.device = system.GetKeyboard(); event.type = InputEventType::ButtonDown; event.control = 32;
    ASSERT_TRUE(system.SubmitEvent(event)); system.FinishEventPump(); user.Update(time); system.EndFrame();
    constexpr int updates = 2000;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < updates; ++i) { time += 1.0 / 120; system.FinishEventPump(); user.Update(time); system.EndFrame(); }
    const auto microseconds = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count() / updates;
    std::printf("[Input performance] 128 active actions, %d updates: %.2f us/update (no timing gate; use NullusInputBenchmark for allocation checks)\n", updates, microseconds);
    RecordProperty("steady_active_actions_us", std::to_string(microseconds));
    EXPECT_EQ(user.GetPendingFixedEventCount(), 0u); EXPECT_TRUE(user.GetActionEvents().empty());
    EXPECT_TRUE(user.GetActionState(user.FindAction(definition.actions.back().id)).actuated);
}
namespace
{
Guid Id(const char* label) { return Guid::NewDeterministic(label); }
ControlPart Key(uint16_t key) { return {{DeviceKind::Keyboard, ControlKind::Button, key}, {1}}; }
InputDefinition ButtonDefinition()
{
    InputDefinition definition;
    definition.id = Id("test.input");
    ActionDefinition action;
    action.id = Id("jump"); action.name = "Jump";
    definition.actions.push_back(action);
    ContextDefinition context;
    context.id = Id("gameplay"); context.name = "Gameplay";
    BindingDefinition binding;
    binding.id = Id("jump.space"); binding.action = action.id; binding.parts = {Key(32)};
    context.bindings.push_back(binding);
    definition.contexts.push_back(context);
    return definition;
}
class InputActionTests : public testing::Test
{
protected:
    double time = 0;
    InputSystem system{[this] { return time; }};
    InputUser user{system, 1};
    void Load(InputDefinition data)
    {
        std::vector<std::string> diagnostics;
        auto definition = CompiledInput::Compile(std::move(data), diagnostics);
        ASSERT_TRUE(definition) << (diagnostics.empty() ? "" : diagnostics.front());
        user.SetDefinition(definition);
        user.PushContext(Id("gameplay"));
    }
    void KeyEvent(uint16_t key, bool down)
    {
        InputEvent event;
        event.device = system.GetKeyboard(); event.control = key;
        event.timestamp = time;
        event.type = down ? InputEventType::ButtonDown : InputEventType::ButtonUp;
        ASSERT_TRUE(system.SubmitEvent(event));
    }
    void Update(double advance = 0)
    {
        time += advance;
        user.Update(time);
        system.EndFrame();
    }
    size_t Count(ActionPhase phase) const
    {
        return std::count_if(user.GetActionEvents().begin(), user.GetActionEvents().end(), [phase](const auto& event) { return event.phase == phase; });
    }
};
}

TEST_F(InputActionTests, MultipleShortPressesAreNotMergedAndFixedEventsAreConsumedExactlyOnce)
{
    Load(ButtonDefinition());
    int calls = 0;
    auto subscription = user.BindAction(Id("jump"), ActionPhase::Performed, [&](const auto&) { ++calls; });
    for (int i = 0; i < 3; ++i) { KeyEvent(32, true); KeyEvent(32, false); }
    Update();
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(Count(ActionPhase::Pressed), 3);
    EXPECT_EQ(Count(ActionPhase::Released), 3);
    const auto pending = user.GetPendingFixedEventCount();
    EXPECT_GT(pending, 6u);
    Update(0.01); // zero fixed ticks: no dropped events, no second dynamic dispatch
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(user.GetPendingFixedEventCount(), pending);
    const auto& tick = user.BeginFixedTick(1, 0.02);
    EXPECT_EQ(std::count_if(tick.events.begin(), tick.events.end(), [](const auto& event) { return event.phase == ActionPhase::Pressed; }), 3);
    EXPECT_EQ(tick.events.size(), pending);
    EXPECT_TRUE(user.BeginFixedTick(2, 0.02).events.empty());
    EXPECT_TRUE(user.BeginFixedTick(3, 0.02).events.empty());
    EXPECT_THROW(user.BeginFixedTick(3, 0.02), std::invalid_argument);
}

TEST_F(InputActionTests, ReleasingOneOfTwoHeldBindingsDoesNotReleaseTheAction)
{
    auto data = ButtonDefinition();
    auto second = data.contexts[0].bindings[0]; second.id = Id("jump.enter"); second.parts = {Key(257)};
    data.contexts[0].bindings.push_back(second);
    Load(data);
    KeyEvent(32, true); KeyEvent(257, true); KeyEvent(32, false);
    Update();
    EXPECT_EQ(Count(ActionPhase::Pressed), 1);
    EXPECT_EQ(Count(ActionPhase::Released), 0);
    EXPECT_TRUE(user.GetActionState(user.FindAction(Id("jump"))).actuated);
    KeyEvent(257, false); Update();
    EXPECT_EQ(Count(ActionPhase::Released), 1);
}

TEST_F(InputActionTests, ActionHoldAggregatesSourcesAndAdvancesWithoutDeviceEvents)
{
    auto data = ButtonDefinition();
    data.actions[0].interaction = {InteractionKind::Hold, 0.5};
    auto other = data.contexts[0].bindings[0]; other.id = Id("jump.enter"); other.parts = {Key(257)};
    data.contexts[0].bindings.push_back(other);
    Load(data);
    KeyEvent(32, true); Update();
    Update(0.3); KeyEvent(257, true); KeyEvent(32, false); Update();
    Update(0.2);
    EXPECT_EQ(Count(ActionPhase::Performed), 1);
    EXPECT_DOUBLE_EQ(user.GetActionEvents()[0].timestamp, 0.5);
}

TEST_F(InputActionTests, BindingHoldRequiresItsOwnSourceToRemainHeld)
{
    auto data = ButtonDefinition();
    auto& binding = data.contexts[0].bindings[0];
    binding.hasInteraction = true; binding.interaction = {InteractionKind::Hold, 0.5};
    Load(data);
    KeyEvent(32, true); Update(0.3);
    EXPECT_EQ(Count(ActionPhase::Pressed), 0);
    KeyEvent(32, false); Update();
    Update(0.3);
    EXPECT_EQ(Count(ActionPhase::Performed), 0);
    KeyEvent(32, true); Update(); Update(0.5);
    EXPECT_EQ(Count(ActionPhase::Performed), 1);
}

TEST_F(InputActionTests, TapAndRepeatAdvanceAtTheirOwnDeadlines)
{
    auto data = ButtonDefinition();
    data.actions[0].interaction = {InteractionKind::Repeat, 0.3, 0.1};
    Load(data);
    KeyEvent(32, true); Update();
    EXPECT_EQ(Count(ActionPhase::Performed), 1);
    Update(0.61);
    EXPECT_EQ(Count(ActionPhase::Performed), 4);
    KeyEvent(32, false); Update();
    EXPECT_EQ(Count(ActionPhase::Released), 1);
    Update(1);
    EXPECT_EQ(Count(ActionPhase::Performed), 0);
}

TEST_F(InputActionTests, ProcessedOutputDeterminesThresholds)
{
    auto data = ButtonDefinition();
    data.contexts[0].bindings[0].processors.push_back({ProcessorKind::Scale, {0.25f, 1, 1}});
    Load(data);
    KeyEvent(32, true); Update();
    EXPECT_EQ(Count(ActionPhase::Pressed), 0);
    EXPECT_FLOAT_EQ(user.GetActionValue(user.FindAction(Id("jump"))).AsAxis(), 0.25f);
}

TEST_F(InputActionTests, ExactAndSubsetModifierMatchingAndPrimaryAreExplicit)
{
    auto data = ButtonDefinition();
    auto& chord = data.contexts[0].bindings[0].chord;
    chord.matchModifiers = true; chord.modifiers = Modifier::Primary; chord.modifierMatch = ModifierMatch::Exact;
    Load(data);
    user.SetPrimaryModifier(Modifier::Control);
    KeyEvent(341, true); KeyEvent(340, true); KeyEvent(32, true); Update();
    EXPECT_EQ(Count(ActionPhase::Performed), 0);
    KeyEvent(32, false); KeyEvent(340, false); KeyEvent(32, true); Update();
    EXPECT_EQ(Count(ActionPhase::Performed), 1);
}

TEST_F(InputActionTests, ContextDisableCancelsWithoutReleaseAndRequireReleasePreventsGhostInput)
{
    Load(ButtonDefinition());
    KeyEvent(32, true); Update();
    user.DisableContext(Id("gameplay")); Update();
    EXPECT_EQ(Count(ActionPhase::Canceled), 1);
    EXPECT_EQ(Count(ActionPhase::Released), 0);
    user.EnableContext(Id("gameplay")); Update();
    EXPECT_EQ(Count(ActionPhase::Pressed), 0);
    KeyEvent(32, false); Update();
    EXPECT_EQ(Count(ActionPhase::Released), 0);
    KeyEvent(32, true); Update();
    EXPECT_EQ(Count(ActionPhase::Pressed), 1);
}

TEST_F(InputActionTests, IndependentUsersKeepTheirOwnOverridesAndStates)
{
    Load(ButtonDefinition());
    InputUser other(system, 2); other.SetDefinition(user.GetDefinition()); other.PushContext(Id("gameplay"));
    user.SetBindingOverride({Id("jump.space"), OverrideState::Override, {Key(257)}});
    KeyEvent(32, true);
    other.Update(time); user.Update(time);
    EXPECT_TRUE(other.WasActionPressed(other.FindAction(Id("jump"))));
    EXPECT_FALSE(user.WasActionPressed(user.FindAction(Id("jump"))));
    EXPECT_EQ(other.GetEffectiveBinding(Id("jump.space"))[0].path.control, 32);
    EXPECT_EQ(user.GetEffectiveBinding(Id("jump.space"))[0].path.control, 257);
}

TEST_F(InputActionTests, CallbackCanUnsubscribeAndDisableContextWithoutInvalidatingDispatch)
{
    Load(ButtonDefinition());
    InputSubscription first, second;
    int calls = 0;
    first = user.BindAction(Id("jump"), ActionPhase::Performed, [&](const auto&)
    { ++calls; first.Reset(); second.Reset(); user.DisableContext(Id("gameplay")); });
    second = user.BindAction(Id("jump"), ActionPhase::Performed, [&](const auto&) { calls += 100; });
    KeyEvent(32, true); KeyEvent(32, false); KeyEvent(32, true); Update();
    EXPECT_EQ(calls, 1);
    Update();
    EXPECT_FALSE(user.IsContextEnabled(Id("gameplay")));
}

TEST_F(InputActionTests, NewSubscriptionsWaitUntilTheNextDispatchBatchAfterVectorGrowth)
{
    Load(ButtonDefinition());
    std::vector<InputSubscription> added;
    int originalCalls = 0, addedCalls = 0;
    auto original = user.BindAction(Id("jump"), ActionPhase::Performed, [&](const auto&)
    {
        ++originalCalls;
        if (added.empty())
            for (int i = 0; i < 64; ++i)
                added.push_back(user.BindAction(Id("jump"), ActionPhase::Performed, [&](const auto&) { ++addedCalls; }));
    });
    KeyEvent(32, true); KeyEvent(32, false); KeyEvent(32, true); Update();
    EXPECT_EQ(originalCalls, 2); EXPECT_EQ(addedCalls, 0);
    KeyEvent(32, false); KeyEvent(32, true); Update();
    EXPECT_EQ(originalCalls, 3); EXPECT_EQ(addedCalls, 64);
}

TEST_F(InputActionTests, HotReloadInvalidatesHandlesButKeepsSubscriptionsAndDeletedOverrides)
{
    auto data = ButtonDefinition(); Load(data);
    const auto oldHandle = user.FindAction(Id("jump"));
    int calls = 0;
    auto subscription = user.BindAction(Id("jump"), ActionPhase::Performed, [&](const auto&) { ++calls; });
    user.ClearBinding(Id("removed.binding"));
    Load(data);
    EXPECT_FALSE(user.GetActionState(oldHandle).actuated);
    EXPECT_EQ(user.GetUnappliedOverrides(), std::vector<BindingId>{Id("removed.binding")});
    KeyEvent(32, true); Update();
    EXPECT_EQ(calls, 1);
}

TEST_F(InputActionTests, DeltaIsConsumedOnceWhileRateIsSampledEveryFixedTick)
{
    auto data = ButtonDefinition();
    data.actions[0].nature = ValueNature::Delta; data.actions[0].type = ValueType::Vector2;
    auto& binding = data.contexts[0].bindings[0]; binding.nature = ValueNature::Delta;
    binding.parts = {{{DeviceKind::Mouse, ControlKind::PointerDelta}, {1}}};
    Load(data);
    InputEvent event; event.device = system.GetMouse(); event.type = InputEventType::PointerDelta; event.x = 2; event.y = 3;
    system.SubmitEvent(event); system.SubmitEvent(event); Update();
    EXPECT_EQ(user.BeginFixedTick(1, 0.02).actions[0].value.AsVector2(), Maths::Vector2(4, 6));
    EXPECT_EQ(user.BeginFixedTick(2, 0.02).actions[0].value.AsVector2(), Maths::Vector2(0, 0));
}

TEST(InputActionCompilationTests, MouseDeltaAndStickRateCannotBeComparedByAccident)
{
    auto data = ButtonDefinition();
    data.actions[0].nature = ValueNature::Delta;
    data.contexts[0].bindings[0].nature = ValueNature::Rate;
    std::vector<std::string> diagnostics;
    EXPECT_FALSE(CompiledInput::Compile(data, diagnostics));
    EXPECT_FALSE(diagnostics.empty());
}

TEST_F(InputActionTests, PerformedConsumptionCancelsLowerActionAndKeepsOwnershipUntilRelease)
{
    auto data = ButtonDefinition();
    auto hold = data.actions[0]; hold.id = Id("hold"); hold.interaction = {InteractionKind::Hold, 0.5};
    data.actions.push_back(hold);
    auto modal = data.contexts[0]; modal.id = Id("modal"); modal.priority = 100; modal.consumption = Consumption::OnPerformed;
    modal.bindings[0].id = Id("hold.space"); modal.bindings[0].action = hold.id;
    data.contexts.push_back(modal);
    Load(data); user.PushContext(modal.id);
    KeyEvent(32, true); Update();
    EXPECT_TRUE(user.WasActionPressed(user.FindAction(Id("jump"))));
    Update(0.5);
    EXPECT_EQ(Count(ActionPhase::Canceled), 1);
    EXPECT_EQ(Count(ActionPhase::Released), 0);
    EXPECT_FALSE(user.GetActionState(user.FindAction(Id("jump"))).actuated);
    Update(0.1);
    EXPECT_FALSE(user.WasActionPressed(user.FindAction(Id("jump"))));
    user.PopContext(modal.id); Update();
    EXPECT_FALSE(user.WasActionPressed(user.FindAction(Id("jump"))));
    KeyEvent(32, false); KeyEvent(32, true); Update();
    EXPECT_TRUE(user.WasActionPressed(user.FindAction(Id("jump"))));
}

TEST_F(InputActionTests, ResumeImmediatelyCanResumeHeldInputAfterContextActivation)
{
    auto data = ButtonDefinition(); data.contexts[0].reacquire = Reacquire::ResumeImmediately;
    Load(data);
    KeyEvent(32, true); Update();
    user.DisableContext(Id("gameplay")); Update();
    user.EnableContext(Id("gameplay")); Update();
    EXPECT_TRUE(user.WasActionPressed(user.FindAction(Id("jump"))));
}

TEST_F(InputActionTests, SharedActionUsesOnlyTheHighestPriorityMappingGroup)
{
    auto data = ButtonDefinition();
    auto high = data.contexts[0]; high.id = Id("high"); high.priority = 2;
    high.bindings[0].id = Id("high.enter"); high.bindings[0].parts = {Key(257)};
    data.contexts.push_back(high);
    Load(data); user.PushContext(high.id);
    KeyEvent(32, true); Update(); EXPECT_EQ(Count(ActionPhase::Performed), 0);
    KeyEvent(257, true); Update(); EXPECT_EQ(Count(ActionPhase::Performed), 1);
    KeyEvent(32, false); Update(); EXPECT_EQ(Count(ActionPhase::Released), 0);
}

TEST_F(InputActionTests, TapFailureAndReleaseInteractionHaveIndependentEdges)
{
    auto data = ButtonDefinition(); data.actions[0].interaction = {InteractionKind::Tap, 0.2};
    Load(data);
    KeyEvent(32, true); Update(); Update(0.1); KeyEvent(32, false); Update();
    EXPECT_EQ(Count(ActionPhase::Released), 1); EXPECT_EQ(Count(ActionPhase::Performed), 1);
    KeyEvent(32, true); Update(); Update(0.21);
    EXPECT_EQ(Count(ActionPhase::Canceled), 1); EXPECT_EQ(Count(ActionPhase::Released), 0);
    KeyEvent(32, false); Update(); EXPECT_EQ(Count(ActionPhase::Performed), 0);
}

TEST_F(InputActionTests, ChordLossCancelsWithoutReleaseAndDoesNotSynthesizeANewPress)
{
    auto data = ButtonDefinition();
    data.contexts[0].bindings[0].chord.controls = {Key(65).path};
    Load(data);
    KeyEvent(65, true); KeyEvent(32, true); Update(); EXPECT_EQ(Count(ActionPhase::Performed), 1);
    KeyEvent(65, false); Update();
    EXPECT_EQ(Count(ActionPhase::Canceled), 1); EXPECT_EQ(Count(ActionPhase::Released), 0);
    KeyEvent(65, true); Update(); EXPECT_EQ(Count(ActionPhase::Pressed), 0);
}

TEST_F(InputActionTests, PauseDiscardsPendingCommandsAndDoesNotAdvanceHoldTime)
{
    auto data = ButtonDefinition(); data.actions[0].interaction = {InteractionKind::Hold, 0.5};
    Load(data); KeyEvent(32, true); Update(0.2);
    user.SetSuspended(true); Update(5);
    EXPECT_EQ(user.GetPendingFixedEventCount(), 0u);
    user.SetSuspended(false); Update();
    EXPECT_EQ(Count(ActionPhase::Performed), 0);
    KeyEvent(32, false); KeyEvent(32, true); Update(); Update(0.5);
    EXPECT_EQ(Count(ActionPhase::Performed), 1);
}

TEST_F(InputActionTests, FixedRateIsSampledEveryTickWithoutRepeatingPressed)
{
    auto data = ButtonDefinition(); data.actions[0].nature = ValueNature::Rate;
    data.contexts[0].bindings[0].nature = ValueNature::Rate;
    Load(data); KeyEvent(32, true); Update();
    EXPECT_FLOAT_EQ(user.BeginFixedTick(1, 0.02).actions[0].value.AsAxis(), 1);
    const auto& second = user.BeginFixedTick(2, 0.02);
    EXPECT_FLOAT_EQ(second.actions[0].value.AsAxis(), 1); EXPECT_TRUE(second.events.empty());
    EXPECT_FALSE(second.actions[0].pressed);
}

TEST(InputReplayTests, IdenticalOrderedStreamsAndUpdateBoundariesProduceIdenticalCommands)
{
    auto replay = []
    {
        double time = 0;
        InputSystem system([&] { return time; }); InputUser user(system, 1);
        std::vector<std::string> diagnostics;
        auto data = ButtonDefinition(); data.actions[0].interaction = {InteractionKind::Repeat, 0.2, 0.1};
        user.SetDefinition(CompiledInput::Compile(data, diagnostics)); user.PushContext(Id("gameplay"));
        auto submit = [&](bool down)
        {
            InputEvent event; event.device = system.GetKeyboard(); event.control = 32;
            event.type = down ? InputEventType::ButtonDown : InputEventType::ButtonUp; system.SubmitEvent(event);
        };
        submit(true); submit(false); submit(true); user.Update(time); system.EndFrame();
        time = 0.51; user.Update(time); system.EndFrame();
        submit(false); user.Update(time);
        return user.BeginFixedTick(1, 0.02).events;
    };
    auto first = replay(), second = replay();
    ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i)
    {
        EXPECT_EQ(first[i].phase, second[i].phase); EXPECT_EQ(first[i].sequence, second[i].sequence);
        EXPECT_EQ(first[i].sourceSequence, second[i].sourceSequence); EXPECT_EQ(first[i].tick, second[i].tick);
        EXPECT_DOUBLE_EQ(first[i].timestamp, second[i].timestamp); EXPECT_EQ(first[i].value, second[i].value);
    }
}

TEST(InputReplayTests, JournalReplaysProfilesLifecycleTimersAndZeroOrMultipleFixedSteps)
{
    using Op = ReplayOperation;
    std::vector<std::string> diagnostics;
    auto data = ButtonDefinition(); data.actions[0].interaction = {InteractionKind::Hold, .2};
    const auto definition = CompiledInput::Compile(data, diagnostics); ASSERT_TRUE(definition);
    std::vector<InputReplayRecord> journal;
    auto add = [&](Op operation) -> InputReplayRecord& { journal.emplace_back(); journal.back().operation = operation; return journal.back(); };
    add(Op::BeginDomain); add(Op::Definition).definition = definition; add(Op::PushContext).context = Id("gameplay");
    InputSystem source; uint64_t sourceSequence = 100;
    auto key = [&](uint16_t control, bool down, double time)
    {
        auto& record = add(Op::Event); record.event.device = source.GetKeyboard(); record.event.control = control;
        record.event.type = down ? InputEventType::ButtonDown : InputEventType::ButtonUp;
        record.event.timestamp = time; record.event.sequence = ++sourceSequence;
    };
    key(32, true, 0); add(Op::Update); add(Op::EndFrame); // no physical step
    add(Op::Update).time = .2; add(Op::EndFrame); // Hold advances without device events
    add(Op::FixedTick).object = 1; journal.back().time = .02;
    add(Op::FixedTick).object = 2; journal.back().time = .02;
    add(Op::SuspendDomain).enabled = true;
    add(Op::AdvanceTime).time = 4;
    add(Op::SuspendDomain).enabled = false;
    add(Op::Profile).profile.overrides.push_back({Id("jump.space"), OverrideState::Override, {Key(257)}});
    key(32, false, 4); key(257, true, 4); add(Op::Update).time = 4; add(Op::EndFrame);
    add(Op::Update).time = 4.2;
    add(Op::FixedTick).object = 3; journal.back().time = .02;
    add(Op::StopDomain); // preserves devices while retiring the Play epoch
    add(Op::BeginDomain); add(Op::Definition).definition = definition; add(Op::PushContext).context = Id("gameplay");
    add(Op::Update).time = 4.2;
    add(Op::FixedTick).object = 1; journal.back().time = .02;
    std::vector<InputReplayBatch> first, second;
    ASSERT_TRUE(ReplayInput(journal, first, diagnostics)); ASSERT_TRUE(ReplayInput(journal, second, diagnostics));
    ASSERT_EQ(first.size(), second.size());
    int performed = 0; bool sawEmptySecondTick = false, sawOriginalSource = false;
    for (size_t i = 0; i < first.size(); ++i)
    {
        const auto& a = first[i]; const auto& b = second[i];
        EXPECT_EQ(a.frame.tick, b.frame.tick); EXPECT_EQ(a.frame.epoch, b.frame.epoch); EXPECT_EQ(a.mode, b.mode);
        ASSERT_EQ(a.frame.events.size(), b.frame.events.size());
        for (size_t e = 0; e < a.frame.events.size(); ++e)
        {
            const auto& x = a.frame.events[e]; const auto& y = b.frame.events[e];
            EXPECT_EQ(x.sequence, y.sequence); EXPECT_EQ(x.sourceSequence, y.sourceSequence);
            EXPECT_EQ(x.phase, y.phase); EXPECT_EQ(x.tick, y.tick); EXPECT_EQ(x.value, y.value);
            EXPECT_DOUBLE_EQ(x.timestamp, y.timestamp); EXPECT_DOUBLE_EQ(x.realTimestamp, y.realTimestamp);
            if (a.mode == UpdateMode::Fixed && x.phase == ActionPhase::Performed) ++performed;
            sawOriginalSource |= x.sourceSequence == 101;
        }
        if (a.mode == UpdateMode::Fixed && a.frame.tick == 2) { EXPECT_TRUE(a.frame.events.empty()); sawEmptySecondTick = true; }
    }
    EXPECT_EQ(performed, 2); EXPECT_TRUE(sawEmptySecondTick); EXPECT_TRUE(sawOriginalSource);
    EXPECT_TRUE(first.back().frame.events.empty()); // no command from the previous Play
}

TEST(InputReplayTests, LateDialogEventsReachDynamicAndFixedConsumersExactlyOnce)
{
    using Op = ReplayOperation;
    std::vector<std::string> diagnostics;
    const auto definition = CompiledInput::Compile(ButtonDefinition(), diagnostics); ASSERT_TRUE(definition);
    std::vector<InputReplayRecord> journal;
    auto add = [&](Op operation) -> InputReplayRecord& { journal.emplace_back(); journal.back().operation = operation; return journal.back(); };
    add(Op::BeginDomain); add(Op::Definition).definition = definition; add(Op::PushContext).context = Id("gameplay");
    InputSystem source;
    auto key = [&](bool down)
    {
        auto& record = add(Op::Event); record.event.device = source.GetKeyboard(); record.event.control = 32;
        record.event.type = down ? InputEventType::ButtonDown : InputEventType::ButtonUp;
        record.event.timestamp = 0;
    };
    key(true); add(Op::FinishEventPump); add(Op::Update);
    key(false); // Release arrives in the command's native dialog after Update.
    add(Op::EndFrame); // No Fixed step: the first press stays pending.
    key(true); key(false); add(Op::FinishEventPump); add(Op::Update);
    add(Op::FixedTick).object = 1; journal.back().time = .02;
    add(Op::FixedTick).object = 2; journal.back().time = .02;
    add(Op::EndFrame); add(Op::FinishEventPump); add(Op::Update);
    std::vector<InputReplayBatch> first, second;
    ASSERT_TRUE(ReplayInput(journal, first, diagnostics)); ASSERT_TRUE(ReplayInput(journal, second, diagnostics));
    ASSERT_EQ(first.size(), 5u); ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i)
    {
        ASSERT_EQ(first[i].frame.events.size(), second[i].frame.events.size());
        for (size_t e = 0; e < first[i].frame.events.size(); ++e)
        {
            EXPECT_EQ(first[i].frame.events[e].sequence, second[i].frame.events[e].sequence);
            EXPECT_EQ(first[i].frame.events[e].phase, second[i].frame.events[e].phase);
        }
    }
    const auto count = [](const auto& batch, ActionPhase phase)
    { return std::count_if(batch.frame.events.begin(), batch.frame.events.end(), [=](const auto& event) { return event.phase == phase; }); };
    EXPECT_EQ(count(first[0], ActionPhase::Performed), 1);
    EXPECT_EQ(count(first[1], ActionPhase::Performed), 1);
    EXPECT_EQ(count(first[1], ActionPhase::Released), 2);
    EXPECT_EQ(count(first[2], ActionPhase::Performed), 2);
    EXPECT_EQ(count(first[2], ActionPhase::Released), 2);
    EXPECT_TRUE(first[3].frame.events.empty()); EXPECT_TRUE(first[4].frame.events.empty());
}

TEST(InputReplayTests, RecordedDeviceAndWindowIdentitiesRemapWithExplicitDeviceBindings)
{
    using Op = ReplayOperation;
    auto data = ButtonDefinition();
    data.contexts[0].bindings[0].parts = {{{DeviceKind::Gamepad, ControlKind::Button, 0, {777}}, {1}}};
    std::vector<std::string> diagnostics; auto definition = CompiledInput::Compile(data, diagnostics); ASSERT_TRUE(definition);
    std::vector<InputReplayRecord> records;
    auto add = [&](Op operation) -> InputReplayRecord& { records.emplace_back(); records.back().operation = operation; return records.back(); };
    add(Op::AddDevice).device = {DeviceKind::Gamepad, "Recorded pad", "Gamepad"}; records.back().object = 777;
    add(Op::AddWindow).object = 888;
    add(Op::BeginDomain); add(Op::Definition).definition = definition;
    add(Op::PairDevice).object = 777; add(Op::PushContext).context = Id("gameplay");
    auto& event = add(Op::Event).event; event.device = {777}; event.window = {888}; event.control = 0;
    event.type = InputEventType::ButtonDown; event.timestamp = 0; event.sequence = 900;
    add(Op::Update);
    std::vector<InputReplayBatch> result; ASSERT_TRUE(ReplayInput(records, result, diagnostics)); ASSERT_EQ(result.size(), 1u);
    int performed = 0;
    for (const auto& action : result[0].frame.events)
        if (action.phase == ActionPhase::Performed)
        { ++performed; EXPECT_EQ(action.device.value, 777u); EXPECT_EQ(action.window.value, 888u); EXPECT_EQ(action.sourceSequence, 900u); }
    EXPECT_EQ(performed, 1);
}

TEST(InputReplayTests, InvalidJournalDoesNotPublishPartialResults)
{
    InputReplayRecord begin; begin.operation = ReplayOperation::BeginDomain;
    InputReplayRecord tick; tick.operation = ReplayOperation::FixedTick; tick.object = 1; tick.time = .02;
    const std::array records{begin, tick, tick};
    std::vector<InputReplayBatch> result(1); result[0].user = 42; std::vector<std::string> diagnostics;
    EXPECT_FALSE(ReplayInput(records, result, diagnostics)); ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].user, 42u); ASSERT_FALSE(diagnostics.empty());
}

TEST_F(InputActionTests, RebindingIgnoresTheLaunchingMousePressButKeepsLaterOrderedInput)
{
    Load(ButtonDefinition());
    InputEvent launch; launch.device = system.GetMouse(); launch.type = InputEventType::ButtonDown; launch.control = 0;
    system.SubmitEvent(launch);
    RebindingSession session(system, user, Id("jump.space"));
    EXPECT_EQ(session.Update(), RebindingStatus::WaitingForRelease);
    launch.type = InputEventType::ButtonUp; system.SubmitEvent(launch);
    KeyEvent(257, true); KeyEvent(257, false);
    EXPECT_EQ(session.Update(), RebindingStatus::Completed);
    ASSERT_EQ(session.GetResult().parts.size(), 1u);
    EXPECT_EQ(session.GetResult().parts[0].path.control, 257);
    ASSERT_TRUE(session.Apply());
    EXPECT_EQ(user.GetEffectiveBinding(Id("jump.space"))[0].path.control, 257);
}

TEST_F(InputActionTests, CompletingRebindSuppressesCaptureButPreservesLaterPressInSamePump)
{
    Load(ButtonDefinition());
    {
        RebindingSession session(system, user, Id("jump.space"));
        KeyEvent(257, true);
        ASSERT_EQ(session.Update(), RebindingStatus::Completed); ASSERT_TRUE(session.Apply());
        KeyEvent(257, false); KeyEvent(257, true); KeyEvent(257, false);
    }
    Update();
    EXPECT_EQ(Count(ActionPhase::Pressed), 1);
    EXPECT_EQ(Count(ActionPhase::Performed), 1);
    EXPECT_EQ(Count(ActionPhase::Released), 1);
}

TEST_F(InputActionTests, CapturedRebindCanCancelAndConfigurationChangesInvalidateItsResult)
{
    auto data = ButtonDefinition(); Load(data);
    for (int change = 0; change < 3; ++change)
    {
        KeyEvent(257, false); Update();
        RebindingSession session(system, user, Id("jump.space"));
        KeyEvent(257, true); ASSERT_EQ(session.Update(), RebindingStatus::Completed);
        if (change == 0) session.Cancel();
        else if (change == 1)
        {
            data.actions[0].name = "Reloaded during capture"; std::vector<std::string> diagnostics;
            user.SetDefinition(CompiledInput::Compile(data, diagnostics));
        }
        else user.SetProfile({});
        EXPECT_EQ(session.Update(), RebindingStatus::Canceled); EXPECT_FALSE(session.Apply());
        EXPECT_TRUE(user.GetProfile().overrides.empty());
    }
}

TEST_F(InputActionTests, AnalogRebindingRequiresNeutralThenStableActuation)
{
    Load(ButtonDefinition());
    auto gamepad = system.RegisterDevice({DeviceKind::Gamepad, "Virtual", "Gamepad"}); user.PairDevice(gamepad);
    InputEvent axis; axis.device = gamepad; axis.type = InputEventType::Axis; axis.control = 0; axis.x = 0.9;
    system.SubmitEvent(axis);
    RebindingSession session(system, user, Id("jump.space"));
    axis.x = 0.8; system.SubmitEvent(axis); time += 0.1; session.Update();
    EXPECT_NE(session.GetStatus(), RebindingStatus::Completed);
    axis.x = 0; system.SubmitEvent(axis); session.Update();
    axis.x = 0.3; system.SubmitEvent(axis); time += 0.1; session.Update();
    EXPECT_NE(session.GetStatus(), RebindingStatus::Completed);
    axis.x = 0.9; system.SubmitEvent(axis); session.Update(); time += 0.06;
    EXPECT_EQ(session.Update(), RebindingStatus::Completed);
}

TEST_F(InputActionTests, DeferredDynamicCallbacksAndFixedCommandsHaveIndependentConsumers)
{
    Load(ButtonDefinition());
    int dynamic = 0, fixed = 0;
    auto a = user.BindAction(Id("jump"), ActionPhase::Performed, [&](const auto&) { ++dynamic; });
    auto b = user.BindAction(Id("jump"), ActionPhase::Performed, [&](const auto&) { ++fixed; }, UpdateMode::Fixed);
    KeyEvent(32, true); KeyEvent(32, false); user.Update(time, false); system.EndFrame();
    KeyEvent(32, true); KeyEvent(32, false); user.Update(time, false);
    EXPECT_EQ(dynamic, 0); EXPECT_EQ(fixed, 0);
    user.DispatchDynamicEvents(); EXPECT_EQ(dynamic, 2); EXPECT_EQ(fixed, 0);
    user.BeginFixedTick(1, 0.02); EXPECT_EQ(fixed, 2);
    user.DispatchDynamicEvents(); user.BeginFixedTick(2, 0.02);
    EXPECT_EQ(dynamic, 2); EXPECT_EQ(fixed, 2);
}

TEST_F(InputActionTests, ConsecutiveReloadAndProfileBoundariesKeepOneCancellation)
{
    auto data = ButtonDefinition(); Load(data);
    int canceled = 0, released = 0;
    auto a = user.BindAction(Id("jump"), ActionPhase::Canceled, [&](const auto&) { ++canceled; });
    auto b = user.BindAction(Id("jump"), ActionPhase::Released, [&](const auto&) { ++released; });
    KeyEvent(32, true); Update();
    std::vector<std::string> diagnostics;
    user.SetDefinition(CompiledInput::Compile(data, diagnostics));
    user.SetProfile({});
    user.DispatchDynamicEvents();
    EXPECT_EQ(canceled, 1); EXPECT_EQ(released, 0);
    EXPECT_TRUE(user.BeginFixedTick(1, .02).events.empty());
}
TEST(InputDevicePairingTests, PairingAnExistingHeldGamepadHasOneMirrorAndReacquiresAfterRelease)
{
    double time = 0; InputSystem system([&] { return time; });
    const auto pad = system.RegisterDevice({DeviceKind::Gamepad, "Existing pad", "Gamepad"});
    InputEvent event; event.device = pad; event.control = 0; event.type = InputEventType::ButtonDown;
    ASSERT_TRUE(system.SubmitEvent(event));
    InputUser user(system, 1); user.PairDevice(pad);
    auto data = ButtonDefinition(); data.contexts[0].bindings[0].parts = {{{DeviceKind::Gamepad, ControlKind::Button, 0}, {1}}};
    std::vector<std::string> diagnostics; user.SetDefinition(CompiledInput::Compile(data, diagnostics)); user.PushContext(Id("gameplay"));
    user.Update(time); EXPECT_FALSE(user.GetActionState(user.FindAction(Id("jump"))).actuated); system.EndFrame();
    for (int pairing = 0; pairing < 2; ++pairing)
    {
        event.type = InputEventType::ButtonUp; ASSERT_TRUE(system.SubmitEvent(event)); user.Update(time); system.EndFrame();
        event.type = InputEventType::ButtonDown; ASSERT_TRUE(system.SubmitEvent(event)); user.Update(time);
        EXPECT_TRUE(user.WasActionPressed(user.FindAction(Id("jump")))); system.EndFrame();
        user.UnpairDevice(pad); user.PairDevice(pad); user.Update(time);
        EXPECT_FALSE(user.GetActionState(user.FindAction(Id("jump"))).actuated); system.EndFrame();
    }
}

TEST(InputDomainTests, ChangesApplyAtBoundariesAndCallbacksSeeEveryUsersCommittedSnapshot)
{
    InputSystem system; InputDomain domain(system, "Runtime"); auto first = domain.CreateUser(), second = domain.CreateUser();
    std::vector<std::string> diagnostics; auto definition = CompiledInput::Compile(ButtonDefinition(), diagnostics);
    for (auto& user : {first, second}) { user->SetDefinition(definition); user->PushContext(Id("gameplay")); }
    int performed = 0;
    auto subscription = first->BindAction(Id("jump"), ActionPhase::Performed, [&](const auto&)
    {
        EXPECT_TRUE(second->GetActionState(second->FindAction(Id("jump"))).actuated); ++performed;
        domain.EnqueueChange([second] { second->DisableContext(Id("gameplay")); });
    });
    InputEvent event; event.device = system.GetKeyboard(); event.control = 32; event.type = InputEventType::ButtonDown; ASSERT_TRUE(system.SubmitEvent(event));
    domain.Update(system.Now()); EXPECT_EQ(performed, 1); EXPECT_TRUE(second->IsContextEnabled(Id("gameplay")));
    system.EndFrame(); domain.Update(system.Now()); EXPECT_FALSE(second->IsContextEnabled(Id("gameplay")));
    EXPECT_TRUE(second->GetActionState(second->FindAction(Id("jump"))).canceled); EXPECT_EQ(performed, 1);
}
TEST(InputDomainTests, NestedChangesStayAtTheNextBoundaryEvenWhenTheDetachedBatchThrows)
{
    InputSystem system; InputDomain domain(system, "Runtime");
    std::vector<int> order;
    domain.EnqueueChange([&]
    {
        order.push_back(1);
        domain.EnqueueChange([&] { order.push_back(3); });
    });
    domain.EnqueueChange([&] { order.push_back(2); });
    domain.Update(system.Now()); EXPECT_EQ(order, (std::vector<int>{1, 2}));
    domain.Update(system.Now()); EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    domain.EnqueueChange([&]
    {
        domain.EnqueueChange([&] { order.push_back(4); });
        throw std::runtime_error("boundary failure");
    });
    domain.EnqueueChange([&] { order.push_back(99); });
    EXPECT_THROW(domain.Update(system.Now()), std::runtime_error);
    domain.Update(system.Now()); EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4}));
    domain.Update(system.Now()); EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4}));
}

TEST_F(InputActionTests, UiDeviceOwnershipCancelsOnlyItsSourcesAndRequiresPhysicalRelease)
{
    Load(ButtonDefinition()); KeyEvent(32, true); Update();
    user.SetDeviceBlocked(DeviceKind::Keyboard, true); Update();
    EXPECT_TRUE(user.GetActionState(user.FindAction(Id("jump"))).canceled);
    EXPECT_FALSE(user.WasActionReleased(user.FindAction(Id("jump"))));
    EXPECT_TRUE(system.IsButtonDown(system.GetKeyboard(), 32));
    user.SetDeviceBlocked(DeviceKind::Keyboard, false); Update();
    EXPECT_FALSE(user.GetActionState(user.FindAction(Id("jump"))).actuated);
    KeyEvent(32, false); KeyEvent(32, true); Update();
    EXPECT_TRUE(user.WasActionPressed(user.FindAction(Id("jump"))));
}
TEST_F(InputActionTests, RebindingLeaseCannotUndoPauseFocusOrComponentDisable)
{
    Load(ButtonDefinition());
    {
        RebindingSession session(system, user, Id("jump.space")); EXPECT_TRUE(user.IsSuspended());
        user.SetSuspended(true, CancelReason::FocusLost); user.SetEnabled(false);
    }
    EXPECT_TRUE(user.IsSuspended()); EXPECT_FALSE(user.IsEnabled());
    user.SetSuspended(false); EXPECT_FALSE(user.IsSuspended()); EXPECT_FALSE(user.IsEnabled());
    user.SetEnabled(true); KeyEvent(32, true); Update(); EXPECT_TRUE(user.WasActionPressed(user.FindAction(Id("jump"))));
}
TEST_F(InputActionTests, DeltaRequiresNeutralBetweenUpdatesWithoutDroppingFreshMovement)
{
    auto data = ButtonDefinition(); data.actions[0].nature = ValueNature::Delta; data.actions[0].type = ValueType::Vector2;
    auto& binding = data.contexts[0].bindings[0]; binding.nature = ValueNature::Delta;
    binding.parts = {{{DeviceKind::Mouse, ControlKind::PointerDelta}, {1}}}; Load(data);
    InputEvent event; event.device = system.GetMouse(); event.type = InputEventType::PointerDelta; event.x = 3;
    ASSERT_TRUE(system.SubmitEvent(event)); Update();
    user.Cancel(CancelReason::RoutingChanged); system.EndFrame();
    event.x = 5; ASSERT_TRUE(system.SubmitEvent(event)); Update(.02);
    EXPECT_EQ(user.GetActionValue(user.FindAction(Id("jump"))).AsVector2().x, 5);
}
TEST_F(InputActionTests, ConflictsDistinguishAliasesFromOpposingDirectionsAndModifiers)
{
    auto data = ButtonDefinition(); auto& context = data.contexts[0];
    auto other = context.bindings[0]; other.id = Id("back"); other.parts[0].contribution = {-1};
    context.bindings.push_back(other); Load(data);
    const auto target = Id("jump.space"); const auto effective = user.GetEffectiveBinding(target);
    const std::vector<ControlPart> parts(effective.begin(), effective.end());
    EXPECT_EQ(FindBindingConflicts(user, target, parts, {}).size(), 1u);
    auto profile = BindingOverride{other.id, OverrideState::Override, {Key(32)}}; user.SetBindingOverride(profile);
    EXPECT_TRUE(FindBindingConflicts(user, target, parts, {}).empty());
    data.actions.push_back({Id("other-action")}); context.bindings[1].action = Id("other-action"); Load(data);
    Chord required; required.matchModifiers = true; required.modifiers = Modifier::Control;
    required.forbiddenModifiers = Modifier::Alt; required.modifierMatch = ModifierMatch::RequiredSubset;
    profile.chord = required; user.SetBindingOverride(profile);
    Chord exact; exact.matchModifiers = true; exact.modifiers = Modifier::Control | Modifier::Alt; exact.modifierMatch = ModifierMatch::Exact;
    EXPECT_TRUE(FindBindingConflicts(user, target, parts, exact).empty());
    exact.modifiers = Modifier::Control | Modifier::Shift;
    EXPECT_EQ(FindBindingConflicts(user, target, parts, exact).size(), 1u);
}
TEST_F(InputActionTests, TimedInteractionsKeepTheirActuatingSourceAcrossUnrelatedInput)
{
    for (const bool bindingLevel : {false, true})
    {
        auto data = ButtonDefinition();
        if (bindingLevel)
        {
            data.contexts[0].bindings[0].hasInteraction = true;
            data.contexts[0].bindings[0].interaction = {InteractionKind::Hold, .2};
        }
        else data.actions[0].interaction = {InteractionKind::Hold, .2};
        Load(data); KeyEvent(32, true); const auto pressedSource = system.GetLastSequence(); Update(.1);
        KeyEvent(65, true); Update(.05); Update(.1);
        int performed = 0;
        for (const auto& event : user.GetActionEvents())
            if (event.phase == ActionPhase::Performed)
            { ++performed; EXPECT_EQ(event.sourceSequence, pressedSource); EXPECT_EQ(event.control, 32); }
        EXPECT_EQ(performed, 1);
        KeyEvent(32, false); KeyEvent(65, false); Update();
    }
}

TEST_F(InputActionTests, AnInactiveMappingGroupCannotKeepAnExpiredHoldDeadlineAlive)
{
    auto data = ButtonDefinition();
    data.contexts[0].bindings[0].hasInteraction = true;
    data.contexts[0].bindings[0].interaction = {InteractionKind::Hold, 0.2};
    auto high = data.contexts[0]; high.id = Id("high"); high.priority = 10;
    high.bindings[0].id = Id("high.enter"); high.bindings[0].parts = {Key(257)};
    data.contexts.push_back(high);
    Load(data); KeyEvent(32, true); Update(0.1);
    user.PushContext(high.id); Update(0.2);
    EXPECT_EQ(Count(ActionPhase::Performed), 0);
    EXPECT_FALSE(user.GetActionState(user.FindAction(Id("jump"))).actuated);
}
