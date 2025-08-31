/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** GlobalSetterBase.h
** --------------------------------------------------------------------------*/

#pragma once

namespace NLS
{
    namespace meta
    {
        class Argument;

        class GlobalSetterBase
        {
        public:
            virtual ~GlobalSetterBase(void) { }

            virtual void SetValue(const Argument &value) = 0;
        };
    }
}