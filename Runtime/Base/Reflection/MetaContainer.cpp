/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** MetaContainer.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "MetaContainer.h"
#include "MetaManager.h"

namespace NLS::meta
{
    const MetaManager &MetaContainer::GetMeta(void) const
    {
        return m_meta;
    }
}
