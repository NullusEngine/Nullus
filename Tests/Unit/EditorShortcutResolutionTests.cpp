#include <gtest/gtest.h>

#include "Shortcuts/EditorShortcutRegistry.h"
#include "Shortcuts/EditorShortcutResolver.h"
#include "Shortcuts/EditorShortcutService.h"

using namespace NLS::Editor::Shortcuts;
using namespace NLS::Windowing::Inputs;

namespace
{
ShortcutCommand MakeCommand(
    std::string id,
    ShortcutContextId context,
    ShortcutBinding binding,
    int& callCount)
{
    ShortcutCommand command;
    command.id = std::move(id);
    command.displayName = command.id;
    command.category = "Test";
    command.context = std::move(context);
    command.defaultBinding = binding;
    command.execute = [&callCount] { ++callCount; };
    return command;
}
}

TEST(EditorShortcutResolutionTests, RejectsDuplicateCommandIds)
{
    EditorShortcutRegistry registry;
    int callCount = 0;

    EXPECT_TRUE(registry.RegisterCommand(MakeCommand(
        "file.save-scene",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl),
        callCount)));

    EXPECT_FALSE(registry.RegisterCommand(MakeCommand(
        "file.save-scene",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_N, EShortcutModifier::Ctrl),
        callCount)));
}

TEST(EditorShortcutResolutionTests, ExecutesOnlyTopPriorityActiveContextCommand)
{
    EditorShortcutService service;
    bool sceneActive = true;
    bool hierarchyActive = false;
    int globalCalls = 0;
    int sceneCalls = 0;
    int hierarchyCalls = 0;

    service.RegisterContext({ ShortcutContexts::SceneView, "Scene View", 20, "focused-panel", [&] { return sceneActive; } });
    service.RegisterContext({ ShortcutContexts::Hierarchy, "Hierarchy", 10, "focused-panel", [&] { return hierarchyActive; } });
    service.RegisterCommand(MakeCommand("global.frame-selected", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_F), globalCalls));
    service.RegisterCommand(MakeCommand("scene.frame-selected", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_F), sceneCalls));
    service.RegisterCommand(MakeCommand("hierarchy.frame-selected", ShortcutContexts::Hierarchy, ShortcutBinding::FromKey(EKey::KEY_F), hierarchyCalls));

    EXPECT_TRUE(service.ExecuteShortcut(ShortcutBinding::FromKey(EKey::KEY_F)));

    EXPECT_EQ(globalCalls, 0);
    EXPECT_EQ(sceneCalls, 1);
    EXPECT_EQ(hierarchyCalls, 0);
}

TEST(EditorShortcutResolutionTests, SuppressesNormalShortcutsDuringTextInput)
{
    EditorShortcutService service;
    bool textInputActive = true;
    int blockedCalls = 0;
    int allowedCalls = 0;

    service.RegisterContext({ ShortcutContexts::TextInput, "Text Input", 100, "", [&] { return textInputActive; } });

    auto blocked = MakeCommand(
        "file.save-scene",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl),
        blockedCalls);
    service.RegisterCommand(blocked);

    auto allowed = MakeCommand(
        "debug.capture",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_F11),
        allowedCalls);
    allowed.allowDuringTextInput = true;
    service.RegisterCommand(allowed);

    EXPECT_FALSE(service.ExecuteShortcut(ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl)));
    EXPECT_TRUE(service.ExecuteShortcut(ShortcutBinding::FromKey(EKey::KEY_F11)));

    EXPECT_EQ(blockedCalls, 0);
    EXPECT_EQ(allowedCalls, 1);
}

TEST(EditorShortcutResolutionTests, RegistersDefaultEditorShortcutCommandIds)
{
    EditorShortcutService service;
    int callCount = 0;

    service.RegisterCommand(MakeCommand("file.new-scene", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_N, EShortcutModifier::Ctrl), callCount));
    service.RegisterCommand(MakeCommand("file.save-scene", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl), callCount));
    service.RegisterCommand(MakeCommand("file.save-scene-as", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl | EShortcutModifier::Shift), callCount));
    service.RegisterCommand(MakeCommand("editor.play", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_F5), callCount));
    service.RegisterCommand(MakeCommand("editor.stop", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_ESCAPE), callCount));
    service.RegisterCommand(MakeCommand("debug.renderdoc.capture-next-frame", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_F11), callCount));
    service.RegisterCommand(MakeCommand("debug.renderdoc.open-latest-capture", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_F11, EShortcutModifier::Ctrl), callCount));
    service.RegisterCommand(MakeCommand("scene-view.fly-forward", ShortcutContexts::SceneViewFlyMode, ShortcutBinding::FromKey(EKey::KEY_W), callCount));
    service.RegisterCommand(MakeCommand("scene-view.fly-backward", ShortcutContexts::SceneViewFlyMode, ShortcutBinding::FromKey(EKey::KEY_S), callCount));
    service.RegisterCommand(MakeCommand("scene-view.fly-left", ShortcutContexts::SceneViewFlyMode, ShortcutBinding::FromKey(EKey::KEY_A), callCount));
    service.RegisterCommand(MakeCommand("scene-view.fly-right", ShortcutContexts::SceneViewFlyMode, ShortcutBinding::FromKey(EKey::KEY_D), callCount));
    service.RegisterCommand(MakeCommand("scene-view.fly-up", ShortcutContexts::SceneViewFlyMode, ShortcutBinding::FromKey(EKey::KEY_E), callCount));
    service.RegisterCommand(MakeCommand("scene-view.fly-down", ShortcutContexts::SceneViewFlyMode, ShortcutBinding::FromKey(EKey::KEY_Q), callCount));
    service.RegisterCommand(MakeCommand("scene-view.view-tool", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_Q), callCount));
    service.RegisterCommand(MakeCommand("scene-view.move-tool", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_W), callCount));
    service.RegisterCommand(MakeCommand("scene-view.rotate-tool", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_E), callCount));
    service.RegisterCommand(MakeCommand("scene-view.scale-tool", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_R), callCount));
    service.RegisterCommand(MakeCommand("scene-view.rect-tool", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_T), callCount));
    service.RegisterCommand(MakeCommand("scene-view.transform-tool", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_Y), callCount));
    service.RegisterCommand(MakeCommand("scene-view.toggle-pivot-position", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_Z), callCount));
    service.RegisterCommand(MakeCommand("scene-view.toggle-pivot-orientation", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_X), callCount));
    service.RegisterCommand(MakeCommand("edit.delete-selected-actor", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_DELETE), callCount));

    EXPECT_EQ(service.GetBindingDisplayText("file.new-scene"), "Ctrl + N");
    EXPECT_EQ(service.GetBindingDisplayText("file.save-scene"), "Ctrl + S");
    EXPECT_EQ(service.GetBindingDisplayText("file.save-scene-as"), "Ctrl + Shift + S");
    EXPECT_EQ(service.GetBindingDisplayText("editor.play"), "F5");
    EXPECT_EQ(service.GetBindingDisplayText("editor.stop"), "Esc");
    EXPECT_EQ(service.GetBindingDisplayText("debug.renderdoc.capture-next-frame"), "F11");
    EXPECT_EQ(service.GetBindingDisplayText("debug.renderdoc.open-latest-capture"), "Ctrl + F11");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.fly-forward"), "W");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.fly-backward"), "S");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.fly-left"), "A");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.fly-right"), "D");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.fly-up"), "E");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.fly-down"), "Q");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.view-tool"), "Q");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.move-tool"), "W");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.rotate-tool"), "E");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.scale-tool"), "R");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.rect-tool"), "T");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.transform-tool"), "Y");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.toggle-pivot-position"), "Z");
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.toggle-pivot-orientation"), "X");
    EXPECT_EQ(service.GetBindingDisplayText("edit.delete-selected-actor"), "Delete");
}

TEST(EditorShortcutResolutionTests, ListsCommandsBySearchText)
{
    EditorShortcutService service;
    int callCount = 0;
    service.RegisterCommand(MakeCommand("file.save-scene", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl), callCount));
    service.RegisterCommand(MakeCommand("debug.capture", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_F11), callCount));

    const auto fileMatches = service.SearchCommands("save");
    const auto shortcutMatches = service.SearchCommands("F11");

    ASSERT_EQ(fileMatches.size(), 1u);
    EXPECT_EQ(fileMatches[0]->id, "file.save-scene");
    ASSERT_EQ(shortcutMatches.size(), 1u);
    EXPECT_EQ(shortcutMatches[0]->id, "debug.capture");
}

TEST(EditorShortcutResolutionTests, ExecutesOnlyAvailableCommands)
{
    EditorShortcutService service;
    bool available = false;
    int callCount = 0;
    auto command = MakeCommand(
        "editor.stop",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_ESCAPE),
        callCount);
    command.availability = [&available] { return available; };
    service.RegisterCommand(command);

    EXPECT_FALSE(service.ExecuteShortcut(ShortcutBinding::FromKey(EKey::KEY_ESCAPE)));
    EXPECT_EQ(callCount, 0);

    available = true;
    EXPECT_TRUE(service.ExecuteShortcut(ShortcutBinding::FromKey(EKey::KEY_ESCAPE)));
    EXPECT_EQ(callCount, 1);
}
