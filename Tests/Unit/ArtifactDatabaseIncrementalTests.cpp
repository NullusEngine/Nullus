#include <gtest/gtest.h>
#include "Assets/ArtifactDatabasePatch.h"

namespace
{
namespace ca=NLS::Core::Assets;
ca::ArtifactManifest Manifest(ca::AssetId source,const std::string& target,const std::string& value)
{
    ca::ArtifactManifest result;result.sourceAssetId=source;result.targetPlatform=target;result.importerId="test";
    result.primarySubAssetKey="mesh:Main";
    const auto hash=ca::ToHexString(ca::ComputeSha256(reinterpret_cast<const uint8_t*>(value.data()),value.size()));
    result.subAssets.push_back({source,"mesh:Main",ca::ArtifactType::Mesh,"mesh",target,"Library/Artifacts/"+hash.substr(0,2)+"/"+hash,"sha256:"+hash,value});
    result.dependencies.push_back({ca::AssetDependencyKind::SourceFileHash,"Assets/Model.obj",value});
    return result;
}
std::filesystem::path DatabasePath()
{return std::filesystem::path(NLS_ROOT_DIR)/"build/artifact-patch-tests"/NLS::Guid::New().ToString();}
}

TEST(ArtifactDatabaseIncrementalTests, SourceTransactionsPreserveOtherSourcesAndTargets)
{
    const auto path=DatabasePath();const auto a=ca::AssetId::New(),b=ca::AssetId::New();
    const auto original=Manifest(a,"editor","a"),otherTarget=Manifest(a,"win64-dx12","cooked"),other=Manifest(b,"editor","b");
    ca::ArtifactDatabase initial;initial.UpsertManifest(original,"Assets/A.obj",ca::ArtifactRecordStatus::UpToDate);
    initial.UpsertManifest(otherTarget,"Assets/A.obj",ca::ArtifactRecordStatus::UpToDate);
    initial.UpsertManifest(other,"Assets/B.obj",ca::ArtifactRecordStatus::UpToDate);ASSERT_TRUE(initial.Save(path));
    auto expected=ca::ReadArtifactDatabaseManifest(path,a,"editor");ASSERT_TRUE(expected);
    auto changedOther=Manifest(b,"editor","b-new");
    ASSERT_TRUE(ca::WriteArtifactDatabaseManifest(path,changedOther,"Assets/B.obj",ca::ArtifactRecordStatus::UpToDate));
    auto changed=Manifest(a,"editor","a-new");
    auto field=Manifest(a,"editor","df").subAssets.front();field.subAssetKey="distancefield:mesh:Main";
    field.artifactType=ca::ArtifactType::MeshDistanceField;field.loaderId="mesh-distance-field";changed.subAssets.push_back(std::move(field));
    std::string error;ASSERT_TRUE(ca::WriteArtifactDatabaseManifest(path,changed,"Assets/A.obj",ca::ArtifactRecordStatus::UpToDate,&*expected,&error))<<error;
    ca::ArtifactDatabase complete;ASSERT_TRUE(complete.Load(path))<<complete.GetLastError();
    const auto first=complete.BuildManifestForSource(a,"editor"),second=complete.BuildManifestForSource(b,"editor"),target=complete.BuildManifestForSource(a,"win64-dx12");
    ASSERT_TRUE(first && second && target);EXPECT_EQ(first->subAssets.size(),2u);
    ASSERT_NE(first->FindPrimaryArtifact(),nullptr);ASSERT_NE(second->FindPrimaryArtifact(),nullptr);ASSERT_NE(target->FindPrimaryArtifact(),nullptr);
    EXPECT_EQ(first->FindPrimaryArtifact()->contentHash,changed.subAssets.front().contentHash);EXPECT_EQ(second->FindPrimaryArtifact()->contentHash,changedOther.subAssets.front().contentHash);
    EXPECT_EQ(target->FindPrimaryArtifact()->contentHash,otherTarget.subAssets.front().contentHash);
    EXPECT_FALSE(ca::WriteArtifactDatabaseManifest(path,original,"Assets/A.obj",ca::ArtifactRecordStatus::UpToDate,&*expected,&error));
    const auto unchanged=ca::ReadArtifactDatabaseManifest(path,a,"editor");ASSERT_TRUE(unchanged);
    ASSERT_NE(unchanged->FindPrimaryArtifact(),nullptr);EXPECT_EQ(unchanged->FindPrimaryArtifact()->contentHash,changed.subAssets.front().contentHash);
}

TEST(ArtifactDatabaseIncrementalTests, ShrinkingDependenciesAndInjectedFailureRemainAtomic)
{
    const auto path=DatabasePath();auto original=Manifest(ca::AssetId::New(),"editor","old");
    original.dependencies.push_back({ca::AssetDependencyKind::SourceFileHash,"Assets/More.bin","more"});
    ASSERT_TRUE(ca::WriteArtifactDatabaseManifest(path,original,"Assets/A.obj",ca::ArtifactRecordStatus::UpToDate));
    auto expected=ca::ReadArtifactDatabaseManifest(path,original.sourceAssetId,"editor");ASSERT_TRUE(expected);
    auto changed=original;changed.dependencies.clear();changed.subAssets.front().contentHash="sha256:new";
#if defined(NLS_ENABLE_TEST_HOOKS)
    ca::SetArtifactDatabaseFailNextSaveForTesting(true);
    EXPECT_FALSE(ca::WriteArtifactDatabaseManifest(path,changed,"Assets/A.obj",ca::ArtifactRecordStatus::UpToDate,&*expected));
    auto retained=ca::ReadArtifactDatabaseManifest(path,original.sourceAssetId,"editor");ASSERT_TRUE(retained);
    ASSERT_NE(retained->FindPrimaryArtifact(),nullptr);EXPECT_EQ(retained->dependencies.size(),2u);EXPECT_EQ(retained->FindPrimaryArtifact()->contentHash,original.subAssets.front().contentHash);
#endif
    ASSERT_TRUE(ca::WriteArtifactDatabaseManifest(path,changed,"Assets/A.obj",ca::ArtifactRecordStatus::UpToDate,&*expected));
    ca::ArtifactDatabase complete;ASSERT_TRUE(complete.Load(path))<<complete.GetLastError();
    auto result=complete.BuildManifestForSource(original.sourceAssetId,"editor");ASSERT_TRUE(result);
    ASSERT_NE(result->FindPrimaryArtifact(),nullptr);EXPECT_TRUE(result->dependencies.empty());EXPECT_EQ(result->FindPrimaryArtifact()->contentHash,"sha256:new");
}
