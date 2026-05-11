#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef INCLUDE_NLOHMANN_JSON_FWD_HPP_
#define INCLUDE_NLOHMANN_JSON_FWD_HPP_

namespace nlohmann
{
inline namespace json_abi_v3_11_3
{
    template<typename T = void, typename SFINAE = void>
    struct adl_serializer;

    template<template<typename U, typename V, typename... Args> class ObjectType = std::map,
        template<typename U, typename... Args> class ArrayType = std::vector,
        class StringType = std::string,
        class BooleanType = bool,
        class NumberIntegerType = std::int64_t,
        class NumberUnsignedType = std::uint64_t,
        class NumberFloatType = double,
        template<typename U> class AllocatorType = std::allocator,
        template<typename T, typename SFINAE = void> class JSONSerializer = adl_serializer,
        class BinaryType = std::vector<std::uint8_t>,
        class CustomBaseClass = void>
    class basic_json;

    template<typename RefStringType>
    class json_pointer;

    using json = basic_json<>;

    template<class Key, class T, class IgnoredLess, class Allocator>
    struct ordered_map;

    using ordered_json = basic_json<nlohmann::ordered_map>;
}
}

#endif
