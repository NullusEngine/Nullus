#pragma once

#include "Reflection/Enum.h"
#include "Reflection/Field.h"
#include "Reflection/Function.h"
#include "Reflection/Method.h"
#include "Reflection/Type.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace NLS::Tests::Reflection
{
inline std::string NormalizeRuntimeTypeName(std::string_view typeName)
{
    std::string normalized(typeName);
    constexpr std::string_view QualifiedArrayPrefix = "NLS::Array<";
    if (normalized.rfind(QualifiedArrayPrefix, 0) == 0)
        normalized.erase(0, std::string_view("NLS::").size());

    return normalized;
}

struct TypeExpectation
{
    std::string name;
    std::vector<std::string> requiredMethods;
    std::vector<std::string> requiredStaticMethods;
    std::vector<std::string> requiredFields;
    std::string requiredBase;
};

inline void ExpectReflectedType(const TypeExpectation& expectation)
{
    using NLS::meta::Field;
    using NLS::meta::Function;
    using NLS::meta::Method;
    using NLS::meta::Type;

    const Type type = Type::GetFromName(expectation.name);
    ASSERT_TRUE(type.IsValid()) << "Type was not registered: " << expectation.name;

    if (!expectation.requiredBase.empty())
    {
        const Type baseType = Type::GetFromName(expectation.requiredBase);
        ASSERT_TRUE(baseType.IsValid()) << "Required base type missing: " << expectation.requiredBase;
        EXPECT_TRUE(type.DerivesFrom(baseType))
            << expectation.name << " does not derive from " << expectation.requiredBase;
    }

    for (const std::string& fieldName : expectation.requiredFields)
    {
        const Field& field = type.GetField(fieldName);
        EXPECT_TRUE(field.IsValid())
            << expectation.name << " is missing reflected field " << fieldName;
    }

    for (const std::string& methodName : expectation.requiredMethods)
    {
        const Method& method = type.GetMethod(methodName);
        EXPECT_TRUE(method.IsValid())
            << expectation.name << " is missing reflected method " << methodName;
    }

    for (const std::string& methodName : expectation.requiredStaticMethods)
    {
        const Function& method = type.GetStaticMethod(methodName);
        EXPECT_TRUE(method.IsValid())
            << expectation.name << " is missing reflected static method " << methodName;
    }
}

inline void ExpectFieldTypeName(const NLS::meta::Type& type, std::string_view fieldName, std::string_view expectedTypeName)
{
    const NLS::meta::Field& field = type.GetField(std::string(fieldName));
    ASSERT_TRUE(field.IsValid()) << type.GetName() << " is missing reflected field " << fieldName;
    EXPECT_EQ(NormalizeRuntimeTypeName(field.GetType().GetName()), NormalizeRuntimeTypeName(expectedTypeName))
        << type.GetName() << "." << fieldName << " has unexpected reflected type";
}

inline void ExpectEnumKeys(const NLS::meta::Type& type, std::initializer_list<std::string_view> expectedKeys)
{
    ASSERT_TRUE(type.IsValid()) << "Enum type was not registered";

    const auto keys = type.GetEnum().GetKeys();
    for (const std::string_view key : expectedKeys)
    {
        EXPECT_NE(std::find(keys.begin(), keys.end(), key), keys.end())
            << type.GetName() << " is missing enum key " << key;
    }
}

inline std::string ReadAllText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    EXPECT_TRUE(input.is_open()) << "Failed to open " << path.string();
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

inline void ExpectContains(std::string_view content, std::string_view needle)
{
    EXPECT_NE(content.find(needle), std::string::npos) << "Missing generated fragment: " << needle;
}

inline void ExpectNotContains(std::string_view content, std::string_view needle)
{
    EXPECT_EQ(content.find(needle), std::string::npos) << "Unexpected generated fragment: " << needle;
}
} // namespace NLS::Tests::Reflection
