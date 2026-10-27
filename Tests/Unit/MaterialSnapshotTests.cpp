#include <gtest/gtest.h>
#include <fstream>
#include "Guid.h"
#include "MaterialCompiler/MaterialDependencySnapshot.h"

TEST(MaterialSnapshotTests, ParentIncludeOrAppliedGenerationChangeInvalidatesWithoutActiveDocumentChange)
{
    using namespace NLS::MaterialCompiler;
    std::map<std::string,MaterialDependencySource> sources;
    sources["child"]={"child",{},"child source",{},1,{"parent"}};
    sources["parent"]={"parent",{},"parent source",{},3,{"include"}};
    sources["include"]={"include",{},"hlsl source",{},0,{}};
    const auto provider=[&](const std::string& key)->std::optional<MaterialDependencySource>
    { const auto it=sources.find(key); return it==sources.end()?std::nullopt:std::optional{it->second}; };
    std::string error;
    const auto snapshot=MaterialDependencySnapshot::Capture({"child"},provider,"DXIL;variants=off",error);
    ASSERT_TRUE(snapshot) << error;
    ASSERT_TRUE(snapshot->IsCurrent(provider));
    sources["include"].content="new hlsl";
    EXPECT_FALSE(snapshot->IsCurrent(provider));
    EXPECT_EQ(snapshot->sources.at("include").content,"hlsl source");
    sources["include"].content="hlsl source";
    ++sources["parent"].appliedGeneration;
    EXPECT_FALSE(snapshot->IsCurrent(provider));
    --sources["parent"].appliedGeneration;
    sources["parent"].metadataHash="new compiler flags";
    EXPECT_FALSE(snapshot->IsCurrent(provider));
}

TEST(MaterialSnapshotTests, ContextChangesAffectIdentityAndMissingDependenciesAreErrors)
{
    using namespace NLS::MaterialCompiler;
    const auto provider=[](const std::string& key)->std::optional<MaterialDependencySource>
    { if (key!="shader") return std::nullopt; return MaterialDependencySource{"shader",{},"source",{},0,{}}; };
    std::string error;
    auto first=MaterialDependencySnapshot::Capture({"shader"},provider,"DXIL;keyword=0",error);
    auto second=MaterialDependencySnapshot::Capture({"shader"},provider,"SPIRV;keyword=0",error);
    ASSERT_TRUE(first); ASSERT_TRUE(second);
    EXPECT_NE(first->Id(),second->Id());
    EXPECT_FALSE(MaterialDependencySnapshot::Capture({"missing"},provider,"DXIL",error));
    EXPECT_NE(error.find("missing"),std::string::npos);
}

TEST(MaterialSnapshotTests, FrozenSourceStagingRetainsCapturedBytesAfterOriginalChanges)
{
    using namespace NLS::MaterialCompiler;
    const auto root=std::filesystem::temp_directory_path()/("MaterialSnapshot_"+NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    const auto source=root/"Constants.hlsli";
    {std::ofstream file(source);file << "original";}
    MaterialDependencySnapshot snapshot;
    snapshot.context="DXIL";snapshot.sources["include"]={"include",source,"original",{},1,{}};
    const auto id=snapshot.Id();
    {std::ofstream file(source);file << "changed on disk";}
    std::string error;
    ASSERT_TRUE(snapshot.Stage(root/"cache",error)) << error;
    std::ifstream staged(snapshot.StagePath(root/"cache",source));
    const std::string content{std::istreambuf_iterator<char>(staged),std::istreambuf_iterator<char>()};
    EXPECT_EQ(content,"original"); EXPECT_EQ(snapshot.Id(),id);
    snapshot.context.append("\0\xff\x01",3);
    snapshot.sources["include"].content.append("\0\xfe\x02",3);
    const auto archive=root/"snapshot.json";
    ASSERT_TRUE(snapshot.Save(archive,error))<<error;
    const auto restored=MaterialDependencySnapshot::Load(archive,error);ASSERT_TRUE(restored)<<error;
    EXPECT_EQ(restored->Id(),snapshot.Id());EXPECT_EQ(restored->sources.at("include").content,snapshot.sources.at("include").content);
    staged.close(); std::filesystem::remove_all(root);
}
