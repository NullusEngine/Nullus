/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ArrayWrapper.h
** --------------------------------------------------------------------------*/

#pragma once

#include "BaseDef.h"
#include "ArrayWrapperBase.h"

namespace NLS::meta
{
    class Argument;

    class NLS_BASE_API ArrayWrapper
    {
    public:
        ArrayWrapper(void);
        ~ArrayWrapper(void);

        ArrayWrapper(const ArrayWrapper& rhs) = delete;
        ArrayWrapper& operator=(const ArrayWrapper& rhs) = delete;

        ArrayWrapper(ArrayWrapper&& rhs) noexcept;
        ArrayWrapper& operator=(ArrayWrapper&& rhs) noexcept;

        template<typename T>
        ArrayWrapper(Array<T> &rhs);

        template<typename T>
        ArrayWrapper(const Array<T> &rhs);

        template<typename T, typename Allocator>
        ArrayWrapper(std::vector<T, Allocator> &rhs);

        template<typename T, typename Allocator>
        ArrayWrapper(const std::vector<T, Allocator> &rhs);

        Variant GetValue(size_t index) const;
        void SetValue(size_t index, const Argument &value);

        void Insert(size_t index, const Argument &value);
        void InsertDefault(size_t index);
        void Remove(size_t index);
        void Resize(size_t size);

        size_t Size(void) const;

        bool IsValid(void) const;
        bool IsConst(void) const;
        bool CanSetValue(void) const;
        bool CanInsert(void) const;
        bool CanInsertDefault(void) const;
        bool CanRemove(void) const;
        bool CanResize(void) const;

    private:
        bool m_isConst;

        ArrayWrapperBase *m_base;
    };
}

#include "Impl/ArrayWrapper.hpp"
