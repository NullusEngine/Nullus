/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ArrayWrapperBase.h
** --------------------------------------------------------------------------*/

#pragma once

#include "Array.h"

namespace NLS::meta
{
    class Variant;
    class Argument;

    class ArrayWrapperBase
    {
    public:
        virtual ~ArrayWrapperBase(void) { }

        virtual Variant GetValue(size_t index) = 0;
        virtual void SetValue(size_t index, const Argument &value) = 0;

        virtual void Insert(size_t index, const Argument &value) = 0;
        virtual void InsertDefault(size_t index) = 0;
        virtual void Remove(size_t index) = 0;
        virtual void Resize(size_t size) = 0;

        virtual size_t Size(void) const = 0;

        virtual bool CanSetValue(void) const = 0;
        virtual bool CanInsert(void) const = 0;
        virtual bool CanInsertDefault(void) const = 0;
        virtual bool CanRemove(void) const = 0;
        virtual bool CanResize(void) const = 0;
    };
}
