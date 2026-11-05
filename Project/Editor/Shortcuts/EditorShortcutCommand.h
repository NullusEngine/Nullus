#pragma once

#include <functional>
#include <string>
#include <vector>

#include "EditorShortcutBinding.h"
#include "EditorShortcutContext.h"
#include <Base/Guid.h>

namespace NLS::Editor::Shortcuts
{
    enum class CommandSource { Shortcut, Menu, Toolbar, Script, Automation };
    enum class CommandResult { Completed, Pending, Disabled, TargetExpired, Failed, UnknownCommand };
    struct InvocationContext
    {
        CommandSource source = CommandSource::Shortcut;
        std::string contentPanel;
        Guid target;
        std::vector<Guid> selection;
        uint64_t sourceSequence = 0;
        bool playMode = false;
        int32_t targetInstance = 0;
        std::string targetPath;
    };
    // Display and routing policy shared by commands and continuous input settings.
    // Physical mappings and interactions belong to the .inputactions definition.
    struct ShortcutDescriptor
    {
        std::string id;
        std::string displayName;
        std::string category;
        ShortcutContextId context = ShortcutContexts::Global;
        bool requiredBinding = false;
        bool allowDuringTextInput = false;
        bool allowContextOverride = false;
    };

    struct ShortcutCommand : ShortcutDescriptor
    {
        std::function<bool()> availability;
        std::function<void()> execute;
        std::function<bool(const InvocationContext&)> canExecute;
        std::function<CommandResult(const InvocationContext&)> invoke;
    };

    enum class EShortcutConflictType
    {
        InvalidBinding,
        DuplicateGlobal,
        DuplicateContext,
        GlobalContextCollision
    };

    struct ShortcutConflict
    {
        EShortcutConflictType type = EShortcutConflictType::InvalidBinding;
        ShortcutBinding binding = ShortcutBinding::Unassigned();
        std::vector<std::string> commandIds;
        std::string message;
        bool blocking = true;
    };

}
