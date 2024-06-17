#pragma once

#include <type_traits>
#include <tuple>

namespace NLS
{
// enum class 转换成内置值类型
template<typename E>
constexpr auto UnderlyingValue(E enumerator) noexcept
{
    return static_cast<std::underlying_type_t<E>>(enumerator);
}

// 扩展std::get<idx>(t) 接收enum class类型的idx
template<auto idx, class TupleType>
constexpr decltype(auto) get(TupleType&& t) noexcept
{
    using IndexType = decltype(idx);
    if constexpr (std::is_enum_v<IndexType>)
    {
        return std::get<UnderlyingValue(idx)>(std::forward<TupleType>(t));
    }
    else
    {
        return std::get<idx>(std::forward<TupleType>(t));
    }
}
} // namespace NLS
