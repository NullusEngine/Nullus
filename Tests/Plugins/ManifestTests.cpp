#include <gtest/gtest.h>
#include "Plugins/Manifest.h"
#include <Json/json.hpp>
using namespace NLS::Plugins;
TEST(PluginSemVersion, ValidatesEveryBranchAndSupportsSeparatedComparators)
{
    EXPECT_TRUE(Satisfies(SemVersion::Parse("1.5.0"),">= 1.0.0 < 2.0.0"));
    EXPECT_THROW(Satisfies(SemVersion::Parse("1.0.0"),"* || invalid"),std::invalid_argument);
    EXPECT_THROW(Satisfies(SemVersion::Parse("1.0.0"),"* ||"),std::invalid_argument);
    EXPECT_NO_THROW(SemVersion::Parse("1.0.0-99999999999999999999999999999999"));
    EXPECT_THROW(SemVersion::Parse("1.0.0-01"),std::invalid_argument);
}
namespace
{
PluginManifest Candidate(const std::string& id, const std::string& version, nlohmann::json dependencies = nlohmann::json::array())
{
    nlohmann::json j{{"schemaVersion",1},{"id",id},{"name",id},{"version",version},
        {"engineVersion","^1.0.0"},{"pluginApiVersion",1},{"modules",nlohmann::json::array()},{"dependencies",dependencies}};
    auto result = PluginManifest::Parse(j.dump()); result.source = "fixture:" + id; return result;
}
}
TEST(PluginManifest, ReadsRealExampleAndRetainsUnknownFields)
{
    auto p = PluginManifest::Read(std::filesystem::path(NLS_PLUGIN_EXAMPLES) / "RuntimeSample/plugin.json");
    EXPECT_EQ(p.modules.size(), 1u); EXPECT_EQ(p.content.size(), 1u);
    auto j = nlohmann::json::parse(p.raw); j["futureField"] = true;
    auto future = PluginManifest::Parse(j.dump()); EXPECT_EQ(future.warnings.size(), 1u);
    EXPECT_TRUE(nlohmann::json::parse(future.raw).at("futureField").get<bool>());
    j["modules"][0]["futureModuleOption"]="retained"; j["content"][0]["futureImporterOption"]=17;
    j["dependencies"].push_back({{"id","org.future.dependency"},{"version","^1.0.0"},{"futureConstraint",true}});
    future=PluginManifest::Parse(j.dump()); EXPECT_EQ(future.warnings.size(),4u);
    EXPECT_EQ(nlohmann::json::parse(future.raw),j);
    j["modules"][0].erase("type"); EXPECT_THROW(PluginManifest::Parse(j.dump()),std::exception);
}
TEST(PluginManifest, RejectsSchemaIdentityTypeAndTraversal)
{
    auto j = nlohmann::json::parse(PluginManifest::Read(std::filesystem::path(NLS_PLUGIN_EXAMPLES) / "RuntimeSample/plugin.json").raw);
    auto bad = j; bad["schemaVersion"] = 2; EXPECT_THROW(PluginManifest::Parse(bad.dump()), std::exception);
    bad = j; bad["id"] = "Invalid ID"; EXPECT_THROW(PluginManifest::Parse(bad.dump()), std::exception);
    bad = j; bad["modules"][0]["type"] = "Mystery"; EXPECT_THROW(PluginManifest::Parse(bad.dump()), std::exception);
    bad = j; bad["modules"][0]["binary"] = "../outside.dll"; EXPECT_THROW(PluginManifest::Parse(bad.dump()), std::exception);
    bad = j; bad.erase("name"); EXPECT_THROW(PluginManifest::Parse(bad.dump()), std::exception);
}
TEST(SemVer, OrderingRangesAndPrerelease)
{
    auto v = SemVersion::Parse("1.2.3");
    EXPECT_TRUE(Satisfies(v,"^1.0.0")); EXPECT_TRUE(Satisfies(v,">=1.2.0 <2.0.0"));
    EXPECT_FALSE(Satisfies(v,"~1.1.0")); EXPECT_FALSE(Satisfies(SemVersion::Parse("0.2.4"),"^0.2.5"));
    EXPECT_TRUE(Satisfies(SemVersion::Parse("1.3.0-beta.2"),">=1.3.0-beta.1 <1.3.0"));
    EXPECT_FALSE(Satisfies(SemVersion::Parse("1.3.0-beta.2"),"^1.2.0"));
    EXPECT_LT(SemVersion::Parse("1.0.0-beta.2"),SemVersion::Parse("1.0.0-beta.11"));
    EXPECT_EQ(SemVersion::Parse("1.0.0+abc"),SemVersion::Parse("1.0.0+def"));
    EXPECT_THROW(SemVersion::Parse("1.01.0"),std::exception);
}
TEST(PluginResolver, DeterministicBacktrackingAndLockedPreference)
{
    auto a = Candidate("org.a","1.0.0", {{{"id","org.b"},{"version","^1.0.0"}}});
    auto b1 = Candidate("org.b","1.0.0"), b2 = Candidate("org.b","1.2.0");
    auto result = ResolvePlugins({{"org.a","1.0.0",{}}},{a,b2,b1},"Windows",{{"org.b","1.0.0"}});
    ASSERT_TRUE(result.succeeded) << result.error; ASSERT_EQ(result.ordered.size(),2u);
    EXPECT_EQ(result.ordered[0].id,"org.b"); EXPECT_EQ(result.ordered[0].version.String(),"1.0.0");
    auto again = ResolvePlugins({{"org.a","1.0.0",{}}},{b1,a,b2},"Windows",{{"org.b","1.0.0"}});
    ASSERT_TRUE(again.succeeded); EXPECT_EQ(again.ordered[0].version.String(),"1.0.0");
}
TEST(PluginResolver, RejectsCyclesConflictsAndAmbiguousSources)
{
    auto a = Candidate("org.a","1.0.0", {{{"id","org.b"},{"version","^1.0.0"}}});
    auto b = Candidate("org.b","1.0.0", {{{"id","org.a"},{"version","^1.0.0"}}});
    auto result = ResolvePlugins({{"org.a","1.0.0",{}}},{a,b},"Windows");
    EXPECT_FALSE(result.succeeded); EXPECT_NE(result.error.find("org.a -> org.b -> org.a"),std::string::npos);
    EXPECT_FALSE(ResolvePlugins({{"org.a","1.0.0",{}}},{a,Candidate("org.b","2.0.0")},"Windows").succeeded);
    auto duplicate = a; duplicate.source = "different";
    EXPECT_FALSE(ResolvePlugins({{"org.a","1.0.0",{}}},{a,duplicate,b},"Windows").succeeded);
}
