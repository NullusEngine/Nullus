#pragma once
#include "EditorShortcutService.h"
#include <Core/ServiceLocator.h>

namespace NLS::Editor::Shortcuts
{
inline CommandResult InvokeEditorCommand(const std::string& id, const InvocationContext& context)
{
    if (!NLS::Core::ServiceLocator::Contains<EditorShortcutService>()) return CommandResult::UnknownCommand;
    return NLS::Core::ServiceLocator::Get<EditorShortcutService>().TryExecuteCommand(id, context);
}
inline CommandResult InvokeEditorCommand(const std::string& id, CommandSource source, std::string contentPanel = {})
{
    if (!NLS::Core::ServiceLocator::Contains<EditorShortcutService>()) return CommandResult::UnknownCommand;
    InvocationContext context;
    context.source = source;
    context.contentPanel = std::move(contentPanel);
    return NLS::Core::ServiceLocator::Get<EditorShortcutService>().TryExecuteCommand(id, context);
}
}
