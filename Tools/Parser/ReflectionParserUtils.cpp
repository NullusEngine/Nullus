/* ----------------------------------------------------------------------------
** Copyright (c) 2024 Fredrik A. Kristiansen, All Rights Reserved.
**
** ReflectionParserUtils.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

std::string join_strings(const std::vector<std::string> &strings, const std::string &delimiter)
{
    std::string result;

    for (size_t i = 0; i < strings.size(); ++i)
    {
        result += strings[i];

        if (i < strings.size() - 1)
            result += delimiter;
    }

    return result;
}