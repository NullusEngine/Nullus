/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ObjectWrapper.h
** --------------------------------------------------------------------------*/

#pragma once

#include <type_traits>

#include "BaseDef.h"
#include "VariantBase.h"

#include "Object/Object.h"
#include "Reflection/TypeID.h"

namespace NLS::meta
{
    class Type;

    class NLS_BASE_API ObjectWrapper : public VariantBase
    {
    public:
        ObjectWrapper(NLS::Object* instance, Type type);

        Type GetType(void) const override;
        void *GetPtr(void) const override;
        template <typename T>
        T& GetObjectPointerAs(void) const
        {
            static_assert(std::is_pointer_v<T>, "ObjectWrapper pointer access requires a pointer type.");
            // Variant::GetValue<T>() returns T&, so Object pointer values use
            // thread-local scratch storage. Callers should copy pointer values.
            static thread_local T pointer = nullptr;
            pointer = dynamic_cast<T>(m_object);
            return pointer;
        }

        int ToInt(void) const override;
        bool ToBool(void) const override;
        float ToFloat(void) const override;
        double ToDouble(void) const override;
        std::string ToString(void) const override;

        VariantBase *Clone(void) const override;

    private:
        NLS::Object* m_object;
        TypeID m_typeID;
        bool m_typeIsArray;
    };
}
