/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** RuntimeMetaProperties.h
** --------------------------------------------------------------------------*/

#pragma once

#include <string>

#include "MetaProperty.h"
#include "Type.h"
#include "Reflection/RuntimeMetaProperties.generated.h"

/** @brief Ensures associative enum values are serialized as their literal value.
 */
class SerializeAsNumber : public NLS::meta::MetaProperty
{
};

namespace NLS::meta
{
enum class SerializationFieldIntent
{
    Value,
    Transient,
    OwnedReference,
    ObjectReference,
    AssetReference
};

CLASS(SerializationIntent) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()

    SerializationIntent() = default;
    explicit SerializationIntent(SerializationFieldIntent p_intent)
        : intent(p_intent)
    {
    }

    SerializationFieldIntent GetIntent() const
    {
        return intent;
    }

    SerializationFieldIntent intent = SerializationFieldIntent::Value;
};

CLASS(SerializeField) : public SerializationIntent
{
public:
    GENERATED_BODY()

    SerializeField()
        : SerializationIntent(SerializationFieldIntent::Value)
    {
    }
};

CLASS(Transient) : public SerializationIntent
{
public:
    GENERATED_BODY()

    Transient()
        : SerializationIntent(SerializationFieldIntent::Transient)
    {
    }
};

CLASS(OwnedReference) : public SerializationIntent
{
public:
    GENERATED_BODY()

    OwnedReference()
        : SerializationIntent(SerializationFieldIntent::OwnedReference)
    {
    }
};

CLASS(ObjectReference) : public SerializationIntent
{
public:
    GENERATED_BODY()

    ObjectReference()
        : SerializationIntent(SerializationFieldIntent::ObjectReference)
    {
    }
};

CLASS(AssetReference) : public SerializationIntent
{
public:
    GENERATED_BODY()

    AssetReference()
        : SerializationIntent(SerializationFieldIntent::AssetReference)
    {
    }
};

CLASS(EditorOnly) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()

    bool IsEditorOnly() const
    {
        return true;
    }
};

CLASS(RuntimeOnly) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()

    bool IsRuntimeOnly() const
    {
        return true;
    }
};

CLASS(FormerlySerializedAs) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()

    FormerlySerializedAs() = default;
    explicit FormerlySerializedAs(const char* p_name)
        : name(p_name ? p_name : "")
    {
    }

    std::string name;
};

CLASS(StableTypeName) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()

    StableTypeName() = default;
    explicit StableTypeName(const char* p_name)
        : name(p_name ? p_name : "")
    {
    }

    std::string name;
};

CLASS(FormerlyTypeName) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()

    FormerlyTypeName() = default;
    explicit FormerlyTypeName(const char* p_name)
        : name(p_name ? p_name : "")
    {
    }

    std::string name;
};

CLASS(ComponentMenu) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()

    ComponentMenu() = default;
    explicit ComponentMenu(const char* p_path)
        : path(p_path ? p_path : "")
    {
    }

    std::string path;
};
} // namespace NLS::meta
