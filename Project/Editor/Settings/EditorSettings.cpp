#include "Settings/EditorSettings.h"

#include <Reflection/ReflectionDatabase.h>

namespace NLS::Editor::Settings
{
EditorDebugDrawSettingsObject& EditorSettings::GetDebugDrawSettingsObject()
{
    static EditorDebugDrawSettingsObject object;
    return object;
}

EditorSceneToolSettingsObject& EditorSettings::GetSceneToolSettingsObject()
{
    static EditorSceneToolSettingsObject object;
    return object;
}

EditorRenderingSettingsObject& EditorSettings::GetRenderingSettingsObject()
{
    static EditorRenderingSettingsObject object;
    return object;
}

void EditorSettings::RegisterSettingObjects(EditorSettingsRegistry& p_registry)
{
    meta::ReflectionDatabase::Instance();
    p_registry.Register({
        "editor.debug-draw",
        "Debug Draw",
        "Editor/Debugging",
        EditorSettingPersistenceScope::User,
        [] { return meta::Variant(GetDebugDrawSettingsObject(), meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(EditorDebugDrawSettingsObject)
    });
    p_registry.Register({
        "editor.scene-tools",
        "Scene Tools",
        "Editor/Scene View",
        EditorSettingPersistenceScope::User,
        [] { return meta::Variant(GetSceneToolSettingsObject(), meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(EditorSceneToolSettingsObject)
    });
    p_registry.Register({
        "editor.rendering",
        "Rendering",
        "Editor/Rendering",
        EditorSettingPersistenceScope::User,
        [] { return meta::Variant(GetRenderingSettingsObject(), meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(EditorRenderingSettingsObject)
    });
}
}
