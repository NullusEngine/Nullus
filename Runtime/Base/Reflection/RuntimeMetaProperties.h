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

#define NLS_META_PROPERTY_CLONE(type)                           \
    NLS::meta::MetaProperty* CloneMetaProperty() const override \
    {                                                           \
        return new type(*this);                                 \
    }

/** @brief Ensures associative enum values are serialized as their literal value.
 */
class SerializeAsNumber : public NLS::meta::MetaProperty
{
public:
    NLS_META_PROPERTY_CLONE(SerializeAsNumber)
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
    NLS_META_PROPERTY_CLONE(SerializationIntent)

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
    NLS_META_PROPERTY_CLONE(SerializeField)

    SerializeField()
        : SerializationIntent(SerializationFieldIntent::Value)
    {
    }
};

CLASS(Transient) : public SerializationIntent
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(Transient)

    Transient()
        : SerializationIntent(SerializationFieldIntent::Transient)
    {
    }
};

CLASS(OwnedReference) : public SerializationIntent
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(OwnedReference)

    OwnedReference()
        : SerializationIntent(SerializationFieldIntent::OwnedReference)
    {
    }
};

CLASS(ObjectReference) : public SerializationIntent
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(ObjectReference)

    ObjectReference()
        : SerializationIntent(SerializationFieldIntent::ObjectReference)
    {
    }
};

CLASS(AssetReference) : public SerializationIntent
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(AssetReference)

    AssetReference()
        : SerializationIntent(SerializationFieldIntent::AssetReference)
    {
    }
};

CLASS(EditorOnly) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(EditorOnly)

    bool IsEditorOnly() const
    {
        return true;
    }
};

CLASS(RuntimeOnly) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(RuntimeOnly)

    bool IsRuntimeOnly() const
    {
        return true;
    }
};

CLASS(FormerlySerializedAs) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(FormerlySerializedAs)

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
    NLS_META_PROPERTY_CLONE(StableTypeName)

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
    NLS_META_PROPERTY_CLONE(FormerlyTypeName)

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
    NLS_META_PROPERTY_CLONE(ComponentMenu)

    ComponentMenu() = default;
    explicit ComponentMenu(const char* p_path)
        : path(p_path ? p_path : "")
    {
    }

    std::string path;
};

CLASS(RequiresRestart) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(RequiresRestart)

    bool IsRestartRequired() const
    {
        return true;
    }
};

CLASS(Range) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(Range)

    Range() = default;
    Range(float p_min, float p_max)
    {
        min = p_min;
        max = p_max;
    }

    float min = 0.0f;
    float max = 1.0f;
};
} // namespace NLS::meta

#undef NLS_META_PROPERTY_CLONE
