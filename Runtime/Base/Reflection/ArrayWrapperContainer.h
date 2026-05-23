/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ArrayWrapperContainer.h
** --------------------------------------------------------------------------*/

#pragma once

#include "ArrayWrapperBase.h"

namespace NLS::meta
{
    class Variant;
    class Argument;

    template<typename T, typename ContainerType = Array<T>>
    class ArrayWrapperContainer : public ArrayWrapperBase
    {
    public:
        ArrayWrapperContainer(ContainerType &a);

        Variant GetValue(size_t index) override;
        void SetValue(size_t index, const Argument &value) override;

        void Insert(size_t index, const Argument &value) override;
        void InsertDefault(size_t index) override;
        void Remove(size_t index) override;
        void Resize(size_t size) override;

        size_t Size(void) const override;

        bool CanSetValue(void) const override;
        bool CanInsert(void) const override;
        bool CanInsertDefault(void) const override;
        bool CanRemove(void) const override;
        bool CanResize(void) const override;

    private:
        ContainerType &m_array;
    };
}

#include "Impl/ArrayWrapperContainer.hpp"
