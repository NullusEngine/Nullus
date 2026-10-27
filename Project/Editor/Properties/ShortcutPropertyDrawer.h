#pragma once
#include "Shortcuts/EditorShortcutBinding.h"
#include <Reflection/ExternalReflectionRegistration.h>
namespace NLS::Editor::Shortcuts
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Shortcuts::ShortcutBinding)
}
namespace NLS::Windowing::Inputs
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::Windowing::Inputs::EKey)
}

namespace NLS::Editor::Properties
{
void RegisterShortcutPropertyDrawer();
bool ApplyShortcutBinding(const std::string& commandId, const Shortcuts::ShortcutBinding& binding, std::string& error);
} // namespace NLS::Editor::Properties
