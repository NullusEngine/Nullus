#include <gtest/gtest.h>
#include <Input/InputDomain.h>
#include <Core/Inputs/InputSerialization.h>
#include <Core/ServiceLocator.h>
#include <Scripting/InputScriptApi.h>
#include <Scripting/ScriptEngineApi.h>
#include <Scripting/ScriptFieldSerialization.h>
#include <Engine/Components/PlayerInputComponent.h>
#include <Engine/GameObject.h>
#include "Inputs/InputCodeGenerator.h"
#include "Inputs/InputActionReferencePicker.h"
#include <filesystem>
#include <fstream>

namespace
{
using namespace NLS;
using namespace NLS::Input;
InputDefinition Definition()
{
    InputDefinition data; data.id = Guid::NewDeterministic("usage.asset");
    ActionDefinition jump; jump.id = Guid::NewDeterministic("usage.jump"); jump.name = "Jump"; data.actions.push_back(jump);
    ContextDefinition map; map.id = Guid::NewDeterministic("usage.gameplay"); map.name = "Gameplay";
    BindingDefinition binding; binding.id = Guid::NewDeterministic("usage.space"); binding.action = jump.id;
    binding.parts = {{{DeviceKind::Keyboard, ControlKind::Button, 32}, {1}}}; map.bindings.push_back(binding);
    data.contexts.push_back(map); data.defaultContext = map.id; return data;
}
std::shared_ptr<const CompiledInput> Compile(const InputDefinition& data)
{
    std::vector<std::string> errors; return CompiledInput::Compile(data, errors);
}
class InputUsageTests : public testing::Test
{
protected:
    InputSystem system;
    InputDomain domain{system, "Usage"};
    InputDefinition data = Definition();
    std::shared_ptr<InputUser> first;
    std::filesystem::path root;
    InputDomain* previous = nullptr;
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() / ("nullus-input-usage-" + Guid::New().ToString());
        std::filesystem::create_directories(root);
        if (Core::ServiceLocator::Contains<InputDomain>()) previous = &Core::ServiceLocator::Get<InputDomain>();
        Core::ServiceLocator::Provide(domain); Scripting::ScriptEngineApi::ResetPlayState(1);
        first = domain.CreateUser(); first->SetDefinition(Compile(data)); first->PairDevice(system.GetKeyboard()); first->PushContext(data.defaultContext);
    }
    void TearDown() override
    {
        Scripting::InputScriptApi::Reset(); Scripting::ScriptEngineApi::StopAcceptingPlayRequests();
        if (previous) Core::ServiceLocator::Provide(*previous); else Core::ServiceLocator::Remove<InputDomain>();
        std::error_code ec; std::filesystem::remove_all(root, ec);
    }
    void Press(uint16_t key)
    {
        InputEvent event; event.type = InputEventType::ButtonDown; event.device = system.GetKeyboard(); event.control = key;
        ASSERT_TRUE(system.SubmitEvent(event)); domain.Update(system.Now());
    }
};
TEST_F(InputUsageTests, NamesPathsAndAmbiguousNamesResolveWithoutChangingIds)
{
    const auto id = data.actions[0].id;
    EXPECT_EQ(first->FindAction("Gameplay/Jump").index, first->FindAction(id).index);
    EXPECT_EQ(first->FindAction("gameplay/jump").index, first->FindAction(id).index);
    EXPECT_FALSE(first->FindAction("Missing/Jump"));
    auto other = data.actions[0]; other.id = Guid::New(); data.actions.push_back(other);
    first->SetDefinition(Compile(data));
    EXPECT_FALSE(first->FindAction("Jump"));
    EXPECT_TRUE(first->FindAction("Gameplay/Jump")); // only the original is bound here
    EXPECT_TRUE(first->FindAction(id));
}
TEST_F(InputUsageTests, ScriptLookupReturnsCanonicalStableIdAndSurvivesRename)
{
    uint64_t high = 0, low = 0; std::string error;
    ASSERT_TRUE(Scripting::InputScriptApi::Command(first->GetId(), 20, "Gameplay/Jump", 0, high, error));
    ASSERT_TRUE(Scripting::InputScriptApi::Command(first->GetId(), 21, "Gameplay/Jump", 0, low, error));
    Guid::Bytes bytes{};
    for (int i = 0; i < 8; ++i) { bytes[i] = uint8_t(high >> ((7 - i) * 8)); bytes[i + 8] = uint8_t(low >> ((7 - i) * 8)); }
    EXPECT_EQ(Guid(bytes), data.actions[0].id);
    data.actions[0].name = "Leap"; first->SetDefinition(Compile(data));
    Press(32); Scripting::ScriptInputValue state;
    ASSERT_TRUE(Scripting::InputScriptApi::Read(first->GetId(), Guid(bytes).ToString().c_str(), 0, 0, state));
    EXPECT_NE(state.flags & 2, 0u);
    ASSERT_TRUE(Scripting::InputScriptApi::Command(first->GetId(), 20, "Jump", 0, high, error)); EXPECT_EQ(high, 0u);
}
TEST_F(InputUsageTests, NativeNamedSubscriptionKeepsItsStableIdentityAfterRename)
{
    int performed = 0, missing = 0;
    auto subscription = first->BindAction("Gameplay/Jump", ActionPhase::Performed, [&](const auto&) { ++performed; });
    auto absent = first->BindAction("Missing", ActionPhase::Performed, [&](const auto&) { ++missing; });
    data.actions[0].name = "Leap"; first->SetDefinition(Compile(data)); Press(32);
    EXPECT_EQ(performed, 1); EXPECT_EQ(missing, 0);
}
TEST_F(InputUsageTests, PersistentReferenceValidatesDefinitionAndKeepsPlayerStateSeparate)
{
    auto second = domain.CreateUser(); second->SetDefinition(Compile(data)); second->PairDevice(system.GetKeyboard()); second->PushContext(data.defaultContext);
    second->SetBindingOverride({data.contexts[0].bindings[0].id, OverrideState::Override, {{{DeviceKind::Keyboard, ControlKind::Button, 65}, {1}}}});
    const auto reference = data.id.ToString() + "/" + data.actions[0].id.ToString();
    Press(32); Scripting::ScriptInputValue state;
    ASSERT_TRUE(Scripting::InputScriptApi::Read(first->GetId(), reference.c_str(), 0, 0, state)); EXPECT_NE(state.flags & 2, 0u);
    ASSERT_TRUE(Scripting::InputScriptApi::Read(second->GetId(), reference.c_str(), 0, 0, state)); EXPECT_EQ(state.flags & 3, 0u);
    const auto wrong = Guid::New().ToString() + "/" + data.actions[0].id.ToString();
    ASSERT_TRUE(Scripting::InputScriptApi::Read(first->GetId(), wrong.c_str(), 0, 0, state)); EXPECT_EQ(state.flags, 0u);
}
TEST_F(InputUsageTests, ReferenceRoundTripsThroughExistingScriptFieldSerialization)
{
    const auto reference = data.id.ToString() + "/" + data.actions[0].id.ToString();
    Scripting::ScriptTypeDescriptor type; Scripting::ScriptFieldDescriptor field;
    field.id = 19; field.name = "jump"; field.type.kind = Scripting::ScriptValueKind::String; field.type.name = "Nullus.InputActionReference";
    type.fields.push_back(field); Scripting::SerializedScriptFields fields{{19, reference}}, decoded;
    Scripting::OrphanScriptFields orphans; std::string text;
    ASSERT_TRUE(Scripting::ScriptFieldSerialization::Serialize(fields, orphans, text).Succeeded());
    ASSERT_TRUE(Scripting::ScriptFieldSerialization::Deserialize(text, &type, decoded, orphans).Succeeded());
    EXPECT_EQ(std::get<std::string>(decoded.at(19)), reference);
}
TEST_F(InputUsageTests, DeletedActionStillDeliversCanceledToPersistentSubscribers)
{
    const auto action = data.actions[0].id;
    const auto reference = data.id.ToString() + "/" + action.ToString();
    Press(32);
    Scripting::ScriptInputValue event; uint64_t cursor = 0;
    while (Scripting::InputScriptApi::Read(first->GetId(), reference.c_str(), 2, cursor, event) && (event.flags & 32)) cursor = event.sequence;
    data.actions.clear(); data.contexts[0].bindings.clear(); first->SetDefinition(Compile(data)); domain.Update(system.Now());
    ASSERT_TRUE(Scripting::InputScriptApi::Read(first->GetId(), reference.c_str(), 2, cursor, event));
    ASSERT_NE(event.flags & 32, 0u); EXPECT_EQ(event.phase, uint32_t(ActionPhase::Canceled));
    EXPECT_EQ(event.reason, uint32_t(CancelReason::DefinitionChanged));
    ASSERT_TRUE(Scripting::InputScriptApi::Read(first->GetId(), reference.c_str(), 0, 0, event)); EXPECT_EQ(event.flags, 0u);
}
TEST_F(InputUsageTests, PlayerInputComponentExposesItsOwnUser)
{
    Engine::GameObject player("Input player");
    auto* input = player.AddComponent<Engine::Components::PlayerInputComponent>();
    input->OnAwake(); const auto ownUser = input->GetInputUser(); ASSERT_TRUE(ownUser);
    uint64_t user = 0; std::string error;
    ASSERT_TRUE(Scripting::InputScriptApi::Command(0, 22, "", Scripting::NativeObjectHandle::FromInstanceId(input->GetInstanceID()).value, user, error)) << error;
    EXPECT_EQ(user, ownUser->GetId()); EXPECT_NE(user, first->GetId());
    EXPECT_FALSE(Scripting::InputScriptApi::Command(0, 22, "", 0, user, error));
}
TEST_F(InputUsageTests, GeneratedWrapperUsesStableReferencesAndExistingUser)
{
    std::string source, error;
    ASSERT_TRUE(Editor::Inputs::GenerateInputCSharp(data, "Controls", "Game.Input", source, error)) << error;
    EXPECT_NE(source.find("InputUser? user = null"), std::string::npos);
    EXPECT_NE(source.find("GameplayActions"), std::string::npos);
    EXPECT_NE(source.find(data.actions[0].id.ToString()), std::string::npos);
    EXPECT_EQ(source.find("new global::Nullus.InputSystem"), std::string::npos);
    data.actions[0].name = "class";
    ASSERT_TRUE(Editor::Inputs::GenerateInputCSharp(data, "Controls", "", source, error)); EXPECT_NE(source.find("@class"), std::string::npos);
}
TEST_F(InputUsageTests, CodeGenerationRejectsCollisionsAndProtectsUserSource)
{
    auto duplicate = data.actions[0]; duplicate.id = Guid::New(); duplicate.name = "Jump"; data.actions.push_back(duplicate);
    std::string source, error;
    EXPECT_FALSE(Editor::Inputs::GenerateInputCSharp(data, "Controls", "", source, error)); EXPECT_TRUE(source.empty());
    data.actions.pop_back(); const auto asset = root / "Controls.inputactions";
    { std::ofstream file(asset.string() + ".g.cs"); file << "// user source\n"; }
    EXPECT_FALSE(Editor::Inputs::WriteInputCSharp(asset, data, error));
    std::ifstream original(asset.string() + ".g.cs"); std::string firstLine; std::getline(original, firstLine); EXPECT_EQ(firstLine, "// user source");
}
TEST_F(InputUsageTests, PickerReadsProjectAssetsAndRetainsStableReferenceAfterRename)
{
    std::filesystem::create_directories(root / "Input"); const auto asset = root / "Input/Controls.inputactions";
    std::string error; ASSERT_TRUE(Core::Inputs::SaveInputDefinition(asset, data, error));
    std::vector<std::string> diagnostics; const auto before = Editor::Inputs::BuildInputActionChoices(root, diagnostics);
    ASSERT_EQ(before.size(), 1u); EXPECT_TRUE(diagnostics.empty()); EXPECT_NE(before[0].displayName.find("Jump"), std::string::npos);
    data.actions[0].name = "Leap"; ASSERT_TRUE(Core::Inputs::SaveInputDefinition(asset, data, error));
    const auto after = Editor::Inputs::BuildInputActionChoices(root, diagnostics);
    ASSERT_EQ(after.size(), 1u); EXPECT_EQ(after[0].reference, before[0].reference); EXPECT_NE(after[0].displayName.find("Leap"), std::string::npos);
}
TEST_F(InputUsageTests, GeneratedSourceRegeneratesWithoutChangingActionIdentity)
{
    const auto asset = root / "Controls.inputactions"; std::string error;
    ASSERT_TRUE(Editor::Inputs::WriteInputCSharp(asset, data, error)) << error;
    data.actions[0].name = "Leap"; ASSERT_TRUE(Editor::Inputs::WriteInputCSharp(asset, data, error)) << error;
    std::ifstream file(asset.string() + ".g.cs"); const std::string text(std::istreambuf_iterator<char>(file), {});
    EXPECT_NE(text.find("@Leap"), std::string::npos); EXPECT_NE(text.find(data.actions[0].id.ToString()), std::string::npos);
}
}
