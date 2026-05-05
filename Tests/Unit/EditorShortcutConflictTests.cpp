#include <gtest/gtest.h>

#include "Shortcuts/EditorShortcutService.h"

using namespace NLS::Editor::Shortcuts;
using namespace NLS::Windowing::Inputs;

namespace
{
ShortcutCommand Command(
    std::string id,
    ShortcutContextId context,
    ShortcutBinding binding)
{
    ShortcutCommand command;
    command.id = std::move(id);
    command.displayName = command.id;
    command.category = "Test";
    command.context = std::move(context);
    command.defaultBinding = binding;
    command.execute = [] {};
    return command;
}
}

TEST(EditorShortcutConflictTests, BlocksDuplicateGlobalBindings)
{
    EditorShortcutService service;
    service.RegisterCommand(Command("file.save", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl)));
    service.RegisterCommand(Command("file.save-as", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_N, EShortcutModifier::Ctrl)));

    const auto conflicts = service.ValidateBinding("file.save-as", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl));

    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].type, EShortcutConflictType::DuplicateGlobal);
    EXPECT_TRUE(conflicts[0].blocking);
}

TEST(EditorShortcutConflictTests, BlocksDuplicateBindingsInSameContext)
{
    EditorShortcutService service;
    service.RegisterCommand(Command("scene.frame", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_F)));
    service.RegisterCommand(Command("scene.focus", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_G)));

    const auto conflicts = service.ValidateBinding("scene.focus", ShortcutBinding::FromKey(EKey::KEY_F));

    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].type, EShortcutConflictType::DuplicateContext);
}

TEST(EditorShortcutConflictTests, AllowsSameBindingInMutuallyExclusiveContexts)
{
    EditorShortcutService service;
    service.RegisterContext({ ShortcutContexts::SceneView, "Scene View", 20, "focused-panel", [] { return false; } });
    service.RegisterContext({ ShortcutContexts::Hierarchy, "Hierarchy", 20, "focused-panel", [] { return false; } });
    service.RegisterCommand(Command("scene.delete", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_DELETE)));
    service.RegisterCommand(Command("hierarchy.delete", ShortcutContexts::Hierarchy, ShortcutBinding::FromKey(EKey::KEY_DELETE)));

    const auto conflicts = service.ValidateBinding("hierarchy.delete", ShortcutBinding::FromKey(EKey::KEY_DELETE));

    EXPECT_TRUE(conflicts.empty());
}

TEST(EditorShortcutConflictTests, AllowsFlyModeBindingsToMirrorSceneViewToolBindings)
{
    EditorShortcutService service;
    service.RegisterContext({ ShortcutContexts::SceneView, "Scene View", 20, "focused-panel", [] { return false; } });
    service.RegisterContext({ ShortcutContexts::SceneViewFlyMode, "Scene View/Fly Mode", 40, "scene-navigation-mode", [] { return false; } });
    service.RegisterCommand(Command("scene-view.move", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_W)));
    service.RegisterCommand(Command("scene-view.fly-forward", ShortcutContexts::SceneViewFlyMode, ShortcutBinding::FromKey(EKey::KEY_W)));

    const auto conflicts = service.ValidateBinding("scene-view.fly-forward", ShortcutBinding::FromKey(EKey::KEY_W));

    EXPECT_TRUE(conflicts.empty());
}

TEST(EditorShortcutConflictTests, BlocksGlobalContextCollisionWithoutOverride)
{
    EditorShortcutService service;
    service.RegisterCommand(Command("file.save", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl)));
    service.RegisterCommand(Command("scene.special", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_G)));

    const auto conflicts = service.ValidateBinding("scene.special", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl));

    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].type, EShortcutConflictType::GlobalContextCollision);
}

TEST(EditorShortcutConflictTests, AllowsExplicitContextOverrideOfGlobalBinding)
{
    EditorShortcutService service;
    service.RegisterCommand(Command("file.save", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl)));
    auto contextCommand = Command("scene.special", ShortcutContexts::SceneView, ShortcutBinding::FromKey(EKey::KEY_G));
    contextCommand.allowContextOverride = true;
    service.RegisterCommand(contextCommand);

    const auto conflicts = service.ValidateBinding("scene.special", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl));

    EXPECT_TRUE(conflicts.empty());
}

TEST(EditorShortcutConflictTests, BlocksInvalidBindings)
{
    EditorShortcutService service;
    service.RegisterCommand(Command("file.save", ShortcutContexts::Global, ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl)));

    const auto conflicts = service.ValidateBinding("file.save", ShortcutBinding::FromKey(EKey::KEY_LEFT_CONTROL));

    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].type, EShortcutConflictType::InvalidBinding);
}
