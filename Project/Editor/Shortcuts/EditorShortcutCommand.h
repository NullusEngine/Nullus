#pragma once

#include <functional>
#include <string>
#include <vector>

#include "EditorShortcutBinding.h"
#include "EditorShortcutContext.h"

namespace NLS::Editor::Shortcuts
{
    struct ShortcutCommand
    {
        std::string id;
        std::string displayName;
        std::string category;
        ShortcutContextId context = ShortcutContexts::Global;
        ShortcutBinding defaultBinding = ShortcutBinding::Unassigned();
        bool requiredBinding = false;
        bool allowDuringTextInput = false;
        bool allowContextOverride = false;
        std::function<bool()> availability;
        std::function<void()> execute;

        bool IsAvailable() const;
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

    inline bool ShortcutCommand::IsAvailable() const
    {
        return !availability || availability();
    }
}
