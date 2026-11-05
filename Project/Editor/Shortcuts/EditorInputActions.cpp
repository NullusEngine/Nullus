#include "EditorShortcutService.h"
#include <Core/ServiceLocator.h>

namespace NLS::Editor::Shortcuts
{
Input::ActionSnapshot GetEditorInputState(EditorInputAction action)
{
    return NLS::Core::ServiceLocator::Contains<EditorShortcutService>() ?
        NLS::Core::ServiceLocator::Get<EditorShortcutService>().GetInputState(action) : Input::ActionSnapshot{};
}
}
