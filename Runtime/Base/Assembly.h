#pragma once
#include "UDRefl/config.hpp"
namespace NLS
{
class NLS_BASE_API Assembly
{
public:
    static Assembly& Instance();

    template<typename T>
    Assembly& Load()
    {
        T::Initialize();
        return *this;
    }
};
} // namespace NLS