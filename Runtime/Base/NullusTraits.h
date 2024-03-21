#pragma once

#include <type_traits>

// enum class 转换成内置值类型
template<typename E>
constexpr auto ToUType(E enumerator) noexcept
{
    return static_cast<std::underlying_type_t<E>>(enumerator);
}
