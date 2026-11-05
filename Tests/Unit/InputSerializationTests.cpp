#include <gtest/gtest.h>
#include <Core/Inputs/InputSerialization.h>
using namespace NLS;
using namespace NLS::Input;
TEST(InputSerializationTests, DefinitionRoundTripPreservesStableIdsAndProcessing)
{
    InputDefinition data; data.id = Guid::NewDeterministic("input.asset");
    ActionDefinition action; action.id = Guid::NewDeterministic("move"); action.type = ValueType::Vector2;
    action.processors.push_back({ProcessorKind::Normalize}); data.actions.push_back(action);
    ContextDefinition context; context.id = Guid::NewDeterministic("gameplay"); context.priority = 42;
    BindingDefinition binding; binding.id = Guid::NewDeterministic("move.wasd"); binding.action = action.id; binding.composite = Composite::Vector2;
    binding.parts = {{{DeviceKind::Keyboard, ControlKind::Button, 87}, {0, 1}}}; context.bindings.push_back(binding); data.contexts.push_back(context);
    InputDefinition restored; std::vector<std::string> diagnostics;
    const auto encoded = Core::Inputs::SerializeInputDefinition(data);
    ASSERT_TRUE(Core::Inputs::ParseInputDefinition(encoded, restored, diagnostics));
    EXPECT_EQ(restored.id, data.id); EXPECT_EQ(restored.contexts[0].bindings[0].id, binding.id);
    EXPECT_EQ(restored.actions[0].processors[0].kind, ProcessorKind::Normalize);
    EXPECT_EQ(Core::Inputs::SerializeInputDefinition(restored), encoded);
}
TEST(InputSerializationTests, ProfileRoundTripKeepsDefaultOverrideAndExplicitlyUnboundDistinct)
{
    InputProfile data;
    for (int i = 0; i < 3; ++i) data.overrides.push_back({Guid::NewDeterministic("slot" + std::to_string(i)), static_cast<OverrideState>(i), {}});
    data.overrides[1].parts = {{{DeviceKind::Keyboard, ControlKind::Button, 83}, {1}}};
    Chord chord; chord.modifiers = Modifier::Primary; chord.matchModifiers = true; data.overrides[1].chord = chord;
    InputProfile restored; std::vector<std::string> diagnostics;
    auto encoded = Core::Inputs::SerializeInputProfile(data);
    ASSERT_TRUE(Core::Inputs::ParseInputProfile(encoded, restored, diagnostics));
    ASSERT_EQ(restored.overrides.size(), 3u);
    EXPECT_EQ(restored.overrides[0].state, OverrideState::Default);
    EXPECT_EQ(restored.overrides[1].state, OverrideState::Override);
    EXPECT_EQ(restored.overrides[2].state, OverrideState::ExplicitlyUnbound);
    EXPECT_EQ(restored.overrides[1].chord->modifiers, Modifier::Primary);
    EXPECT_EQ(Core::Inputs::SerializeInputProfile(restored), encoded);
}
TEST(InputSerializationTests, MalformedInputIsRejectedWithoutPartialMutation)
{
    InputProfile profile; profile.pointerSensitivity = 2;
    for (const char* malformed : {"{", "[]", "{\"version\":2}", "{\"version\":1,\"overrides\":false}", "{\"version\":1,\"pointerSensitivity\":\"fast\"}"})
    {
        std::vector<std::string> diagnostics;
        EXPECT_FALSE(Core::Inputs::ParseInputProfile(malformed, profile, diagnostics));
        EXPECT_FLOAT_EQ(profile.pointerSensitivity, 2); EXPECT_FALSE(diagnostics.empty());
    }
}
TEST(InputSerializationTests, ShippedEditorDefaultsAreValidAndUseStableCommandBindingIds)
{
    InputDefinition definition; std::vector<std::string> diagnostics;
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "App/Assets/Editor/Input/EditorShortcuts.inputactions";
    ASSERT_TRUE(Core::Inputs::LoadInputDefinition(path,definition,diagnostics)) << (diagnostics.empty()?"":diagnostics.front());
    const auto binding = Guid::NewDeterministic("editor.binding.file.save-scene.primary");
    bool found=false;
    for (const auto& context : definition.contexts) for (const auto& entry : context.bindings) if (entry.id == binding)
    { found=true;EXPECT_EQ(entry.action,Guid::NewDeterministic("editor.action.file.save-scene"));ASSERT_EQ(entry.parts.size(),1u);EXPECT_EQ(entry.parts[0].path.control,83); }
    EXPECT_TRUE(found);
}
TEST(InputSerializationTests, ShippedGameplayKeepsPointerDeltaAndStickRateInSeparateActions)
{
    InputDefinition definition; std::vector<std::string> diagnostics;
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "App/Assets/Engine/Input/DefaultGameplay.inputactions";
    ASSERT_TRUE(Core::Inputs::LoadInputDefinition(path, definition, diagnostics)) << (diagnostics.empty() ? "" : diagnostics.front());
    ASSERT_TRUE(CompiledInput::Compile(definition, diagnostics));
    EXPECT_TRUE(definition.defaultContext.IsValid());
    ASSERT_EQ(definition.actions.size(), 4u);
    EXPECT_EQ(definition.actions[1].nature, ValueNature::Delta); EXPECT_EQ(definition.actions[2].nature, ValueNature::Rate);
}
