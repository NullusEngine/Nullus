/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ArrayWrapper.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "ArrayWrapper.h"

#include "Debug/Assertion.h"

namespace NLS::meta
{
    ArrayWrapper::ArrayWrapper(void)
        : m_isConst( true )
        , m_base( nullptr ) { }

        ArrayWrapper::~ArrayWrapper(void)
        {
            delete m_base;
            m_base = nullptr;
        }

        ArrayWrapper::ArrayWrapper(ArrayWrapper&& rhs) noexcept
            : m_isConst(rhs.m_isConst)
            , m_base(rhs.m_base)
        {
            rhs.m_isConst = true;
            rhs.m_base = nullptr;
        }

        ArrayWrapper& ArrayWrapper::operator=(ArrayWrapper&& rhs) noexcept
        {
            if (this != &rhs)
            {
                delete m_base;
                m_isConst = rhs.m_isConst;
                m_base = rhs.m_base;
                rhs.m_isConst = true;
                rhs.m_base = nullptr;
            }
            return *this;
        }

        Variant ArrayWrapper::GetValue(size_t index) const
        {
            return m_base ? m_base->GetValue( index ) : Variant( );
        }

        void ArrayWrapper::SetValue(size_t index, const Argument &value)
        {
            NLS_ASSERT( !m_isConst, "Array is const." );

            if (m_base)
                m_base->SetValue( index, value );
        }

        void ArrayWrapper::Insert(size_t index, const Argument &value)
        {
            NLS_ASSERT( !m_isConst, "Array is const." );

            if (m_base)
                m_base->Insert( index, value );
        }

        void ArrayWrapper::InsertDefault(size_t index)
        {
            NLS_ASSERT( !m_isConst, "Array is const." );

            if (m_base)
                m_base->InsertDefault( index );
        }

        void ArrayWrapper::Remove(size_t index)
        {
            NLS_ASSERT( !m_isConst, "Array is const." );

            if (m_base)
                m_base->Remove( index );
        }

        void ArrayWrapper::Resize(size_t size)
        {
            NLS_ASSERT( !m_isConst, "Array is const." );

            if (m_base)
                m_base->Resize( size );
        }

        size_t ArrayWrapper::Size(void) const
        {
            return m_base ? m_base->Size( ) : 0;
        }

        bool ArrayWrapper::IsValid(void) const
        {
            return m_base != nullptr;
        }

    bool ArrayWrapper::IsConst(void) const
    {
        return m_isConst;
    }

    bool ArrayWrapper::CanSetValue(void) const
    {
        return !m_isConst && m_base != nullptr && m_base->CanSetValue();
    }

    bool ArrayWrapper::CanInsert(void) const
    {
        return !m_isConst && m_base != nullptr && m_base->CanInsert();
    }

    bool ArrayWrapper::CanInsertDefault(void) const
    {
        return !m_isConst && m_base != nullptr && m_base->CanInsertDefault();
    }

    bool ArrayWrapper::CanRemove(void) const
    {
        return !m_isConst && m_base != nullptr && m_base->CanRemove();
    }

    bool ArrayWrapper::CanResize(void) const
    {
        return !m_isConst && m_base != nullptr && m_base->CanResize();
    }
}
