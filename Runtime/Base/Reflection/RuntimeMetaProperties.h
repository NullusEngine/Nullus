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

// Component constraints are type metadata, so every editor surface and the
// runtime-facing add-component helpers can make the same decision without a
// panel-specific allow-list.
CLASS(DisallowMultipleComponent) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(DisallowMultipleComponent)
};

CLASS(RequireComponent) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(RequireComponent)

    RequireComponent() = default;
    explicit RequireComponent(const char* p_typeNames)
        : typeNames(p_typeNames ? p_typeNames : "")
    {
    }

    // A semicolon-delimited list keeps the reflection macro invocation simple
    // while allowing a component to declare more than one prerequisite.
    std::string typeNames;
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

// Inspector presentation metadata deliberately lives beside reflection rather
// than in an editor widget. Native and generated script fields can therefore
// describe the same surface without acquiring an Editor dependency.
CLASS(DisplayName) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(DisplayName)

    DisplayName() = default;
    explicit DisplayName(const char* p_value)
        : value(p_value ? p_value : "")
    {
    }

    std::string value;
};

CLASS(Tooltip) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(Tooltip)

    Tooltip() = default;
    explicit Tooltip(const char* p_value)
        : value(p_value ? p_value : "")
    {
    }

    std::string value;
};

CLASS(Header) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(Header)

    Header() = default;
    explicit Header(const char* p_value)
        : value(p_value ? p_value : "")
    {
    }

    std::string value;
};

CLASS(Space) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(Space)

    Space() = default;
    explicit Space(float p_value)
        : value(p_value)
    {
    }

    float value = 0.0f;
};

CLASS(Min) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(Min)

    Min() = default;
    explicit Min(float p_value)
        : value(p_value)
    {
    }

    float value = 0.0f;
};

CLASS(Max) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(Max)

    Max() = default;
    explicit Max(float p_value)
        : value(p_value)
    {
    }

    float value = 0.0f;
};

CLASS(Multiline) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(Multiline)
};

CLASS(ReadOnly) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(ReadOnly)
};

CLASS(HideInInspector) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(HideInInspector)
};

CLASS(Flags) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(Flags)
};

CLASS(ColorUsage) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(ColorUsage)
};

CLASS(ReferenceTypeConstraint) : public NLS::meta::MetaProperty
{
public:
    GENERATED_BODY()
    NLS_META_PROPERTY_CLONE(ReferenceTypeConstraint)

    ReferenceTypeConstraint() = default;
    explicit ReferenceTypeConstraint(const char* p_value)
        : value(p_value ? p_value : "")
    {
    }

    std::string value;
};
} // namespace NLS::meta

#undef NLS_META_PROPERTY_CLONE
