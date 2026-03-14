/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Argument.h
** --------------------------------------------------------------------------*/

#pragma once

#include "TypeConfig.h"

#include <type_traits>
#include <vector>

namespace NLS
{
    namespace meta
    {
        class Type;
        class Variant;

        class Argument
        {
        public:
            Argument(void);
            Argument(const Argument &rhs);
            Argument(Variant &obj);
            Argument(const Variant &obj);

            template<typename T, typename = typename std::enable_if<!std::is_same<typename std::decay<T>::type, Argument>::value>::type>
            Argument(const T &data);

            template<typename T, typename = typename std::enable_if<!std::is_same<typename std::decay<T>::type, Argument>::value>::type>
            Argument(T &data);

            Argument &operator=(const Argument &rhs);

            Type GetType(void) const;

            void *GetPtr(void) const;

            template<typename T>
            T &GetValue(void) const;

        private:
            const TypeID m_typeID;
            const bool m_isArray;

            const void *m_data;
        };
    }
}

#include "Impl/Argument.hpp"
