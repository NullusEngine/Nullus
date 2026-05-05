#pragma once

#include <Reflection/Object.h>
#include <Reflection/Type.h>
#include <Reflection/Variant.h>

#include <functional>
#include <string>

namespace NLS::Editor::Settings
{
enum class EditorSettingPersistenceScope
{
    Project,
    User
};

struct EditorSettingObject
{
    std::string id;
    std::string displayName;
    std::string categoryPath;
    EditorSettingPersistenceScope scope = EditorSettingPersistenceScope::User;
    std::function<meta::Variant()> makeVariant;
    meta::Type type = meta::Type::Invalid();

    bool IsValid() const
    {
        return !id.empty() && !displayName.empty() && !categoryPath.empty() && type.IsValid() && makeVariant != nullptr;
    }
};
}
