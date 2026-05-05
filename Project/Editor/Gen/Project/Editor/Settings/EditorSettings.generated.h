#pragma once

#include <type_traits>
#include "Reflection/Type.h"

namespace NLS::meta
{
class ReflectionDatabase;
enum class ReflectionRegistrationPhase;
class Object;
}

#ifdef CURRENT_FILE_ID
#undef CURRENT_FILE_ID
#endif
#define CURRENT_FILE_ID NLS_FID_Project_Editor_Settings_EditorSettings_h

namespace NLS::meta_generated
{
void RegisterType_NLS__Editor__Settings__EditorDebugDrawSettingsObject(NLS::meta::ReflectionDatabase& db, NLS::meta::ReflectionRegistrationPhase phase);

class StaticTypeRegister_NLS__Editor__Settings__EditorDebugDrawSettingsObject
{
public:
    StaticTypeRegister_NLS__Editor__Settings__EditorDebugDrawSettingsObject();
};

#ifdef NLS_FID_Project_Editor_Settings_EditorSettings_h_11_GENERATED_BODY
#undef NLS_FID_Project_Editor_Settings_EditorSettings_h_11_GENERATED_BODY
#endif
#define NLS_FID_Project_Editor_Settings_EditorSettings_h_11_GENERATED_BODY \
    friend void ::NLS::meta_generated::RegisterType_NLS__Editor__Settings__EditorDebugDrawSettingsObject(::NLS::meta::ReflectionDatabase& db, ::NLS::meta::ReflectionRegistrationPhase phase); \
    friend class ::NLS::meta_generated::StaticTypeRegister_NLS__Editor__Settings__EditorDebugDrawSettingsObject;

void RegisterType_NLS__Editor__Settings__EditorSceneToolSettingsObject(NLS::meta::ReflectionDatabase& db, NLS::meta::ReflectionRegistrationPhase phase);

class StaticTypeRegister_NLS__Editor__Settings__EditorSceneToolSettingsObject
{
public:
    StaticTypeRegister_NLS__Editor__Settings__EditorSceneToolSettingsObject();
};

#ifdef NLS_FID_Project_Editor_Settings_EditorSettings_h_39_GENERATED_BODY
#undef NLS_FID_Project_Editor_Settings_EditorSettings_h_39_GENERATED_BODY
#endif
#define NLS_FID_Project_Editor_Settings_EditorSettings_h_39_GENERATED_BODY \
    friend void ::NLS::meta_generated::RegisterType_NLS__Editor__Settings__EditorSceneToolSettingsObject(::NLS::meta::ReflectionDatabase& db, ::NLS::meta::ReflectionRegistrationPhase phase); \
    friend class ::NLS::meta_generated::StaticTypeRegister_NLS__Editor__Settings__EditorSceneToolSettingsObject;


} // namespace NLS::meta_generated
