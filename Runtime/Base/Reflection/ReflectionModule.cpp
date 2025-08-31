/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ReflectionModule.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "ReflectionModule.h"

namespace NLS
{
    namespace meta
    {
        ReflectionModule::ReflectionModule(ReflectionDatabase &db)
            : db( db ) { }
    }
}