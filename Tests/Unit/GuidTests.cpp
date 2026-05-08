#include <gtest/gtest.h>

#include "Guid.h"
#include "GuidReflection.h"
#include "Reflection/Type.h"

#include <string>
#include <unordered_set>

TEST(GuidTests, NewCreatesValidNonEmptyGuid)
{
    const auto guid = NLS::Guid::New();

    EXPECT_TRUE(guid.IsValid());
    EXPECT_NE(guid, NLS::Guid::Empty());
}

TEST(GuidTests, DeterministicCreatesStableGuidFromLabel)
{
    const auto first = NLS::Guid::NewDeterministic("Player.Transform");
    const auto second = NLS::Guid::NewDeterministic("Player.Transform");
    const auto different = NLS::Guid::NewDeterministic("Player.Mesh");

    EXPECT_EQ(first, second);
    EXPECT_NE(first, different);
    EXPECT_TRUE(first.IsValid());
}

TEST(GuidTests, FormatsAsCanonicalLowercaseText)
{
    const auto guid = NLS::Guid::Parse("7B6F2D2E-9F04-48D5-82C8-24D52E9B3A41");

    EXPECT_EQ(guid.ToString(), "7b6f2d2e-9f04-48d5-82c8-24d52e9b3a41");
}

TEST(GuidTests, TryParseRoundTripsCanonicalText)
{
    const auto original = NLS::Guid::NewDeterministic("RoundTrip");
    const auto parsed = NLS::Guid::TryParse(original.ToString());

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, original);
}

TEST(GuidTests, TryParseRejectsInvalidText)
{
    EXPECT_FALSE(NLS::Guid::TryParse("").has_value());
    EXPECT_FALSE(NLS::Guid::TryParse("not-a-guid").has_value());
    EXPECT_FALSE(NLS::Guid::TryParse("7b6f2d2e-9f04-48d5-82c8-24d52e9b3a4").has_value());
    EXPECT_FALSE(NLS::Guid::TryParse("7b6f2d2e-9f04-48d5-82c8-24d52e9b3a4z").has_value());
}

TEST(GuidTests, EmptyGuidIsNotValid)
{
    EXPECT_FALSE(NLS::Guid::Empty().IsValid());
    EXPECT_EQ(NLS::Guid::Empty().ToString(), "00000000-0000-0000-0000-000000000000");
}

TEST(GuidTests, CanBeUsedAsUnorderedSetKey)
{
    std::unordered_set<NLS::Guid> values;
    const auto first = NLS::Guid::NewDeterministic("KeyA");
    const auto second = NLS::Guid::NewDeterministic("KeyB");

    values.insert(first);
    values.insert(second);
    values.insert(first);

    EXPECT_EQ(values.size(), 2u);
    EXPECT_TRUE(values.contains(first));
    EXPECT_TRUE(values.contains(second));
}

TEST(GuidTests, SerializesToJsonString)
{
    const auto guid = NLS::Guid::Parse("7b6f2d2e-9f04-48d5-82c8-24d52e9b3a41");

    NLS::json output = guid;

    ASSERT_TRUE(output.is_string());
    EXPECT_EQ(output.get<std::string>(), "7b6f2d2e-9f04-48d5-82c8-24d52e9b3a41");
}

TEST(GuidTests, DeserializesFromJsonString)
{
    const NLS::json input = "7b6f2d2e-9f04-48d5-82c8-24d52e9b3a41";

    const auto guid = input.get<NLS::Guid>();

    EXPECT_EQ(guid.ToString(), "7b6f2d2e-9f04-48d5-82c8-24d52e9b3a41");
}

TEST(GuidTests, ReflectionRegistersGuidType)
{
    const auto type = NLS::meta::Type::GetFromName("NLS::Guid");

    EXPECT_TRUE(type.IsValid());
}
