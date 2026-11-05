#pragma once
#include "Shortcuts/EditorShortcutBinding.h"
#include <Input/InputAction.h>
namespace NLS::Editor::Properties
{
void RegisterShortcutPropertyDrawer();
bool ApplyShortcutBinding(const std::string& commandId, const Shortcuts::ShortcutBinding& binding, std::string& error);
bool ApplyShortcutSlot(const std::string& commandId, size_t slot, Input::BindingOverride value, std::string& error);
} // namespace NLS::Editor::Properties
