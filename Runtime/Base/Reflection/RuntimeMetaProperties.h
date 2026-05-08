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
