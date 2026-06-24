#include <gtest/gtest.h>

#include "Assets/AssetId.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "Guid.h"
#include "Rendering/SceneRendererMaterialBinding.h"
#include "Serialize/ObjectGraphDocument.h"

#include <lmdb.h>

namespace
{
constexpr const char* kTestArtifactDatabaseSchema = "Nullus.ArtifactDB.LMDB";
constexpr const char* kTestArtifactDatabaseVersion = "3";

NLS::Core::Assets::AssetId MakeAssetId(const char* guid)
{
    return NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
}

NLS::Core::Assets::ImportedArtifact MakeArtifact(
    NLS::Core::Assets::AssetId owner,
    std::string subAssetKey,
    NLS::Core::Assets::ArtifactType type,
    std::string loaderId,
    std::string artifactPath)
{
    return {
        owner,
        std::move(subAssetKey),
        type,
        std::move(loaderId),
        "win64",
        std::move(artifactPath),
        "sha256:" + owner.ToString()
    };
}

std::string RuntimeArtifactPathForHash(const std::string& hash)
{
    return (std::filesystem::path("Artifacts") /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(hash)).generic_string();
}

std::string EditorArtifactPathForHash(const std::string& hash)
{
    return (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(hash)).generic_string();
}

NLS::Core::Assets::ArtifactManifest MakeManifest(
    NLS::Core::Assets::AssetId owner,
    std::string primarySubAssetKey,
    std::initializer_list<NLS::Core::Assets::ImportedArtifact> artifacts)
{
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = owner;
    manifest.targetPlatform = "win64";
    manifest.primarySubAssetKey = std::move(primarySubAssetKey);
    manifest.subAssets.assign(artifacts.begin(), artifacts.end());
    return manifest;
}

std::string EncodeTestRecord(
    const NLS::Core::Assets::AssetId sourceAssetId,
    const std::string& sourcePath,
    const std::string& subAssetKey,
    const NLS::Core::Assets::ArtifactType artifactType,
    const std::string& loaderId,
    const std::string& targetPlatform,
    const std::string& artifactPath,
    const std::string& contentHash,
    const std::string& displayName,
    const std::string& importerId,
    const uint32_t importerVersion,
    const std::string& primarySubAssetKey,
    const size_t dependencyCount,
    const NLS::Core::Assets::ArtifactRecordStatus status)
{
    return sourceAssetId.ToString() + "\t" +
        sourcePath + "\t" +
        subAssetKey + "\t" +
        std::to_string(static_cast<int>(artifactType)) + "\t" +
        loaderId + "\t" +
        targetPlatform + "\t" +
        artifactPath + "\t" +
        contentHash + "\t" +
        displayName + "\t" +
        importerId + "\t" +
        std::to_string(importerVersion) + "\t" +
        primarySubAssetKey + "\t" +
        std::to_string(dependencyCount) + "\t" +
        std::to_string(static_cast<int>(status));
}

bool PutLmdbString(MDB_txn* transaction, const MDB_dbi database, const std::string& key, const std::string& value)
{
    MDB_val keyValue {
        key.size(),
        const_cast<char*>(key.data())
    };
    MDB_val valueData {
        value.size(),
        const_cast<char*>(value.data())
    };
    return mdb_put(transaction, database, &keyValue, &valueData, 0u) == MDB_SUCCESS;
}

bool WriteCorruptArtifactDatabaseWithOneValidRecord(
    const std::filesystem::path& databasePath,
    const NLS::Core::Assets::AssetId validSourceId,
    const std::string& validArtifactPath)
{
    std::filesystem::create_directories(databasePath);

    MDB_env* environment = nullptr;
    if (mdb_env_create(&environment) != MDB_SUCCESS)
        return false;
    if (mdb_env_set_maxdbs(environment, 8u) != MDB_SUCCESS ||
        mdb_env_set_mapsize(environment, 64ull * 1024ull * 1024ull) != MDB_SUCCESS ||
        mdb_env_open(environment, databasePath.string().c_str(), 0u, 0664) != MDB_SUCCESS)
    {
        mdb_env_close(environment);
        return false;
    }

    MDB_txn* transaction = nullptr;
    bool ok = mdb_txn_begin(environment, nullptr, 0u, &transaction) == MDB_SUCCESS;
    MDB_dbi metaDatabase = 0u;
    MDB_dbi recordsDatabase = 0u;
    MDB_dbi dependenciesDatabase = 0u;
    ok = ok &&
        mdb_dbi_open(transaction, "meta", MDB_CREATE, &metaDatabase) == MDB_SUCCESS &&
        mdb_dbi_open(transaction, "records", MDB_CREATE, &recordsDatabase) == MDB_SUCCESS &&
        mdb_dbi_open(transaction, "dependencies", MDB_CREATE, &dependenciesDatabase) == MDB_SUCCESS;
    ok = ok &&
        PutLmdbString(transaction, metaDatabase, "schema", kTestArtifactDatabaseSchema) &&
        PutLmdbString(transaction, metaDatabase, "version", kTestArtifactDatabaseVersion);

    const auto validRecord = EncodeTestRecord(
        validSourceId,
        "Assets/Models/Hero.gltf",
        "mesh:Body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "win64",
        validArtifactPath,
        "sha256:1111111111111111111111111111111111111111111111111111111111111111",
        "Body",
        "scene-model",
        1u,
        "mesh:Body",
        0u,
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ok = ok &&
        PutLmdbString(transaction, recordsDatabase, validSourceId.ToString() + "\nmesh:Body\nwin64", validRecord) &&
        PutLmdbString(transaction, recordsDatabase, "ffffffff-ffff-4fff-8fff-ffffffffffff\nmesh:Broken\nwin64", "not\ta\tvalid\trecord");

    if (ok && mdb_txn_commit(transaction) == MDB_SUCCESS)
        transaction = nullptr;
    else
        ok = false;

    if (transaction != nullptr)
        mdb_txn_abort(transaction);
    mdb_env_close(environment);
    return ok;
}

bool WriteLmdbArtifactDatabase(
    const std::filesystem::path& databasePath,
    const std::function<bool(MDB_txn*, MDB_dbi, MDB_dbi)>& writePayload)
{
    std::filesystem::create_directories(databasePath);

    MDB_env* environment = nullptr;
    if (mdb_env_create(&environment) != MDB_SUCCESS)
        return false;
    if (mdb_env_set_maxdbs(environment, 8u) != MDB_SUCCESS ||
        mdb_env_set_mapsize(environment, 64ull * 1024ull * 1024ull) != MDB_SUCCESS ||
        mdb_env_open(environment, databasePath.string().c_str(), 0u, 0664) != MDB_SUCCESS)
    {
        mdb_env_close(environment);
        return false;
    }

    MDB_txn* transaction = nullptr;
    bool ok = mdb_txn_begin(environment, nullptr, 0u, &transaction) == MDB_SUCCESS;
    MDB_dbi metaDatabase = 0u;
    MDB_dbi recordsDatabase = 0u;
    MDB_dbi dependenciesDatabase = 0u;
    ok = ok &&
        mdb_dbi_open(transaction, "meta", MDB_CREATE, &metaDatabase) == MDB_SUCCESS &&
        mdb_dbi_open(transaction, "records", MDB_CREATE, &recordsDatabase) == MDB_SUCCESS &&
        mdb_dbi_open(transaction, "dependencies", MDB_CREATE, &dependenciesDatabase) == MDB_SUCCESS;
    ok = ok &&
        PutLmdbString(transaction, metaDatabase, "schema", kTestArtifactDatabaseSchema) &&
        PutLmdbString(transaction, metaDatabase, "version", kTestArtifactDatabaseVersion);
    ok = ok && writePayload(transaction, recordsDatabase, dependenciesDatabase);

    if (ok && mdb_txn_commit(transaction) == MDB_SUCCESS)
        transaction = nullptr;
    else
        ok = false;

    if (transaction != nullptr)
        mdb_txn_abort(transaction);
    mdb_env_close(environment);
    return ok;
}
}

TEST(AssetManifestTests, ArtifactStorageFileNamesAreExtensionlessHexContentNames)
{
    const auto name = NLS::Core::Assets::BuildArtifactStorageFileName("Prefab:Assets/Hero.prefab");
    const auto relative = NLS::Core::Assets::BuildArtifactStorageRelativePath(name).generic_string();

    EXPECT_EQ(name.size(), 64u);
    EXPECT_TRUE(NLS::Core::Assets::IsArtifactStorageFileName(name));
    EXPECT_EQ(relative, name.substr(0u, 2u) + "/" + name);
    EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath("Library/Artifacts/" + relative));
    EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath("Artifacts/" + relative));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Library/Artifacts/" + name));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Library/Artifacts/Hero/" + name));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Artifacts/Hero/" + name));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Library/Artifacts/App/Assets/Materials/Hero.mat/" + name));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Artifacts/Assets/Shaders/Hero.shader/" + name));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Artifacts/Packages/Hero/" + name));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Artifacts/Hero.prefab/" + name));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Library/Artifacts/" + name + ".prefab"));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Library/Artifacts/prefab-" + name));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath(name));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Materials/" + name));
    EXPECT_FALSE(NLS::Core::Assets::IsContentStorageArtifactPath("Library/Other/" + name));
    EXPECT_FALSE(NLS::Core::Assets::IsArtifactStorageFileName("materials/" + name));
}

TEST(AssetManifestTests, PortableContentArtifactPathCanBeDerivedFromPhysicalLibraryPath)
{
    const auto name = NLS::Core::Assets::BuildArtifactStorageFileName("Shader:Assets/Hero.shader");
    const auto relative = NLS::Core::Assets::BuildArtifactStorageRelativePath(name);

    EXPECT_EQ(
        NLS::Core::Assets::TryMakePortableContentArtifactPath((std::filesystem::path("Library") / "Artifacts" / relative).generic_string()),
        (std::filesystem::path("Library") / "Artifacts" / relative).generic_string());
    EXPECT_EQ(
        NLS::Core::Assets::TryMakePortableContentArtifactPath(
            (std::filesystem::path("D:/Project") / "Library" / "Artifacts" / relative).generic_string()),
        (std::filesystem::path("Library") / "Artifacts" / relative).generic_string());
    EXPECT_EQ(
        NLS::Core::Assets::TryMakePortableContentArtifactPath(
            (std::filesystem::path("D:/Package") / "Artifacts" / relative).generic_string()),
        (std::filesystem::path("Artifacts") / relative).generic_string());
    EXPECT_TRUE(NLS::Core::Assets::TryMakePortableContentArtifactPath(
        (std::filesystem::path("D:/Project") / "Library" / "Artifacts" / "Assets" / "Hero.shader" / name)
            .generic_string()).empty());
}

TEST(AssetManifestTests, ArtifactDatabaseLoadFailureClearsPreviouslyLoadedAndPartiallyReadRecords)
{
    using namespace NLS::Core::Assets;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_corrupt_artifactdb_partial_load_" + NLS::Guid::New().ToString());
    const auto validSourceId = MakeAssetId("11111111-1111-4111-8111-111111111111");
    const auto staleSourceId = MakeAssetId("22222222-2222-4222-8222-222222222222");

    ArtifactDatabase database;
    auto staleManifest = MakeManifest(
        staleSourceId,
        "mesh:Stale",
        {MakeArtifact(
            staleSourceId,
            "mesh:Stale",
            ArtifactType::Mesh,
            "mesh",
            EditorArtifactPathForHash("2222222222222222222222222222222222222222222222222222222222222222"))});
    database.UpsertManifest(staleManifest, "Assets/Models/Stale.gltf", ArtifactRecordStatus::UpToDate);
    ASSERT_NE(database.Find(staleSourceId, "mesh:Stale", "win64"), nullptr);

    const auto validArtifactPath =
        EditorArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111");
    ASSERT_TRUE(WriteCorruptArtifactDatabaseWithOneValidRecord(root / "ArtifactDB", validSourceId, validArtifactPath));

    EXPECT_FALSE(database.Load(root / "ArtifactDB"));
    EXPECT_EQ(database.GetStats().totalRecords, 0u);
    EXPECT_EQ(database.Find(staleSourceId, "mesh:Stale", "win64"), nullptr);
    EXPECT_EQ(database.Find(validSourceId, "mesh:Body", "win64"), nullptr)
        << "A failed LMDB load must not expose records read before the decode error.";

    std::filesystem::remove_all(root);
}

TEST(AssetManifestTests, ArtifactDatabaseLoadRejectsRecordStorageKeyMismatch)
{
    using namespace NLS::Core::Assets;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_artifactdb_key_mismatch_" + NLS::Guid::New().ToString());
    const auto payloadSourceId = MakeAssetId("11111111-1111-4111-8111-111111111111");
    const auto keySourceId = MakeAssetId("22222222-2222-4222-8222-222222222222");
    const auto artifactPath =
        EditorArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111");

    ASSERT_TRUE(WriteLmdbArtifactDatabase(
        root / "ArtifactDB",
        [&](MDB_txn* transaction, const MDB_dbi recordsDatabase, const MDB_dbi) {
            const auto record = EncodeTestRecord(
                payloadSourceId,
                "Assets/Models/Hero.gltf",
                "mesh:Body",
                ArtifactType::Mesh,
                "mesh",
                "win64",
                artifactPath,
                "sha256:1111111111111111111111111111111111111111111111111111111111111111",
                "Body",
                "scene-model",
                1u,
                "mesh:Body",
                0u,
                ArtifactRecordStatus::UpToDate);
            return PutLmdbString(transaction, recordsDatabase, keySourceId.ToString() + "\nmesh:Body\nwin64", record);
        }));

    ArtifactDatabase database;
    EXPECT_FALSE(database.Load(root / "ArtifactDB"));
    EXPECT_EQ(database.GetStats().totalRecords, 0u);
    EXPECT_EQ(database.Find(payloadSourceId, "mesh:Body", "win64"), nullptr);
    EXPECT_EQ(database.Find(keySourceId, "mesh:Body", "win64"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetManifestTests, ArtifactDatabaseLoadRejectsNonContiguousDependencyIndices)
{
    using namespace NLS::Core::Assets;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_artifactdb_dependency_gap_" + NLS::Guid::New().ToString());
    const auto sourceId = MakeAssetId("11111111-1111-4111-8111-111111111111");
    const auto artifactPath =
        EditorArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111");
    const auto sourceTargetKey = sourceId.ToString() + "\n\nwin64";

    ASSERT_TRUE(WriteLmdbArtifactDatabase(
        root / "ArtifactDB",
        [&](MDB_txn* transaction, const MDB_dbi recordsDatabase, const MDB_dbi dependenciesDatabase) {
            const auto record = EncodeTestRecord(
                sourceId,
                "Assets/Models/Hero.gltf",
                "mesh:Body",
                ArtifactType::Mesh,
                "mesh",
                "win64",
                artifactPath,
                "sha256:1111111111111111111111111111111111111111111111111111111111111111",
                "Body",
                "scene-model",
                1u,
                "mesh:Body",
                2u,
                ArtifactRecordStatus::UpToDate);
            return PutLmdbString(transaction, recordsDatabase, sourceId.ToString() + "\nmesh:Body\nwin64", record) &&
                PutLmdbString(transaction, dependenciesDatabase, sourceTargetKey + "\ncount", "2") &&
                PutLmdbString(
                    transaction,
                    dependenciesDatabase,
                    sourceTargetKey + "\n1",
                    "0\tAssets/Models/Hero.gltf\tsha256:source");
        }));

    ArtifactDatabase database;
    EXPECT_FALSE(database.Load(root / "ArtifactDB"));
    EXPECT_EQ(database.GetStats().totalRecords, 0u);
    EXPECT_EQ(database.Find(sourceId, "mesh:Body", "win64"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetManifestTests, ArtifactDatabaseLoadRejectsMissingZeroDependencyCountEntry)
{
    using namespace NLS::Core::Assets;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_artifactdb_missing_zero_dependency_count_" + NLS::Guid::New().ToString());
    const auto sourceId = MakeAssetId("11111111-1111-4111-8111-111111111111");
    const auto artifactPath =
        EditorArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111");

    ASSERT_TRUE(WriteLmdbArtifactDatabase(
        root / "ArtifactDB",
        [&](MDB_txn* transaction, const MDB_dbi recordsDatabase, const MDB_dbi) {
            const auto record = EncodeTestRecord(
                sourceId,
                "Assets/Models/Hero.gltf",
                "mesh:Body",
                ArtifactType::Mesh,
                "mesh",
                "win64",
                artifactPath,
                "sha256:1111111111111111111111111111111111111111111111111111111111111111",
                "Body",
                "scene-model",
                1u,
                "mesh:Body",
                0u,
                ArtifactRecordStatus::UpToDate);
            return PutLmdbString(transaction, recordsDatabase, sourceId.ToString() + "\nmesh:Body\nwin64", record);
        }));

    ArtifactDatabase database;
    EXPECT_FALSE(database.Load(root / "ArtifactDB"));
    EXPECT_EQ(database.GetStats().totalRecords, 0u);
    EXPECT_EQ(database.Find(sourceId, "mesh:Body", "win64"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetManifestTests, ArtifactDatabaseLoadRejectsMalformedRecordDependencyCount)
{
    using namespace NLS::Core::Assets;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_artifactdb_malformed_record_dependency_count_" + NLS::Guid::New().ToString());
    const auto sourceId = MakeAssetId("11111111-1111-4111-8111-111111111111");
    const auto artifactPath =
        EditorArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111");
    const auto sourceTargetKey = sourceId.ToString() + "\n\nwin64";

    ASSERT_TRUE(WriteLmdbArtifactDatabase(
        root / "ArtifactDB",
        [&](MDB_txn* transaction, const MDB_dbi recordsDatabase, const MDB_dbi dependenciesDatabase) {
            auto record = EncodeTestRecord(
                sourceId,
                "Assets/Models/Hero.gltf",
                "mesh:Body",
                ArtifactType::Mesh,
                "mesh",
                "win64",
                artifactPath,
                "sha256:1111111111111111111111111111111111111111111111111111111111111111",
                "Body",
                "scene-model",
                1u,
                "mesh:Body",
                0u,
                ArtifactRecordStatus::UpToDate);
            const auto statusField = record.rfind('\t');
            if (statusField == std::string::npos)
                return false;
            const auto dependencyCountField = record.rfind('\t', statusField - 1u);
            if (dependencyCountField == std::string::npos)
                return false;
            record.replace(dependencyCountField + 1u, statusField - dependencyCountField - 1u, "0junk");
            return PutLmdbString(transaction, recordsDatabase, sourceId.ToString() + "\nmesh:Body\nwin64", record) &&
                PutLmdbString(transaction, dependenciesDatabase, sourceTargetKey + "\ncount", "0");
        }));

    ArtifactDatabase database;
    EXPECT_FALSE(database.Load(root / "ArtifactDB"));
    EXPECT_EQ(database.GetStats().totalRecords, 0u);
    EXPECT_EQ(database.Find(sourceId, "mesh:Body", "win64"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetManifestTests, ArtifactDatabaseLoadReportsLmdbFailureDetails)
{
    using namespace NLS::Core::Assets;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_artifactdb_lmdb_error_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "ArtifactDB";
    std::filesystem::create_directories(databasePath);

    ArtifactDatabase database;
    EXPECT_FALSE(database.Load(databasePath));
    EXPECT_NE(database.GetLastError().find("mdb_env_open"), std::string::npos);
    EXPECT_NE(database.GetLastError().find("No such file"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetManifestTests, RuntimeManifestIndexesEntriesByGuidAndSubAsset)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("11111111-1111-4111-8111-111111111111");
    RuntimeAssetManifest manifest;
    manifest.schemaVersion = 1u;
    manifest.targetPlatform = "win64";
    manifest.entries.push_back({
        model,
        "mesh:body",
        ArtifactType::Mesh,
        "mesh",
        RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111"),
        "sha256:mesh",
        {}
    });

    RuntimeAssetDatabase database(manifest);

    const auto* entry = database.Resolve(model, "mesh:body");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->artifactPath, RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111"));
    EXPECT_EQ(entry->loaderId, "mesh");
    EXPECT_EQ(database.Resolve(model, "mesh:missing"), nullptr);
}

TEST(AssetManifestTests, RuntimeAssetDatabaseMaintainsIndexedLookupForRuntimeRefs)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("12111111-1111-4111-8111-111111111111");
    RuntimeAssetManifest manifest;
    manifest.schemaVersion = 1u;
    manifest.targetPlatform = "win64";

    for (uint32_t index = 0u; index < 1024u; ++index)
    {
        const auto artifactPath =
            RuntimeArtifactPathForHash(BuildArtifactStorageFileName("RuntimeMaterial:" + std::to_string(index)));
        manifest.entries.push_back({
            model,
            "material:" + std::to_string(index),
            ArtifactType::Material,
            "material",
            artifactPath,
            "sha256:material" + std::to_string(index),
            {}
        });
    }

    RuntimeAssetDatabase database(manifest);

    EXPECT_EQ(database.GetIndexedEntryCount(), manifest.entries.size());
    const auto* entry = database.Resolve({model, "material:1023"});
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->artifactPath, RuntimeArtifactPathForHash(BuildArtifactStorageFileName("RuntimeMaterial:1023")));

    const auto* localEntry = database.ResolveByLocalIdentifierInFile(
        model,
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(model.GetGuid(), "material:1023"));
    ASSERT_NE(localEntry, nullptr);
    EXPECT_EQ(localEntry->artifactPath, RuntimeArtifactPathForHash(BuildArtifactStorageFileName("RuntimeMaterial:1023")));
    EXPECT_EQ(database.ResolveByLocalIdentifierInFile(model, 123456789), nullptr);
}

TEST(AssetManifestTests, ArtifactDatabaseMaintainsSourceIndexForSourceLookups)
{
    using namespace NLS::Core::Assets;

    const auto shader = MakeAssetId("11111111-1111-4111-8111-111111111111");
    const auto mesh = MakeAssetId("22222222-2222-4222-8222-222222222222");

    ArtifactManifest shaderManifest = MakeManifest(
        shader,
        "shader:Hero",
        {
            MakeArtifact(shader, "shader:Hero/Forward#0", ArtifactType::Shader, "ShaderLoader", EditorArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111")),
            MakeArtifact(shader, "shader:Hero/DepthOnly#1", ArtifactType::Shader, "ShaderLoader", EditorArtifactPathForHash("2222222222222222222222222222222222222222222222222222222222222222"))
        });
    ArtifactManifest meshManifest = MakeManifest(
        mesh,
        "mesh:Body",
        {
            MakeArtifact(mesh, "mesh:Body", ArtifactType::Mesh, "MeshLoader", EditorArtifactPathForHash("3333333333333333333333333333333333333333333333333333333333333333"))
        });

    ArtifactDatabase database;
    database.UpsertManifest(shaderManifest, "Assets/Shaders/Hero.shader", ArtifactRecordStatus::UpToDate);
    database.UpsertManifest(meshManifest, "Assets/Models/Hero.fbx", ArtifactRecordStatus::UpToDate);

    EXPECT_EQ(database.GetIndexedSourceRecordCountForTesting(shader), 2u);
    EXPECT_EQ(database.FindBySource(shader).size(), 2u);
    EXPECT_EQ(database.GetIndexedSourceRecordCountForTesting(mesh), 1u);

    database.RemoveSource(shader);

    EXPECT_EQ(database.GetIndexedSourceRecordCountForTesting(shader), 0u);
    EXPECT_TRUE(database.FindBySource(shader).empty());
    EXPECT_EQ(database.GetIndexedSourceRecordCountForTesting(mesh), 1u);
}

TEST(AssetManifestTests, ArtifactDatabaseUpsertReplacesOnlyMatchingSourceTarget)
{
    using namespace NLS::Core::Assets;

    const auto shader = MakeAssetId("31313131-3131-4131-8131-313131313131");
    auto editorManifest = MakeManifest(
        shader,
        "shader:Hero/Forward#0",
        {
            MakeArtifact(shader, "shader:Hero/Forward#0", ArtifactType::Shader, "ShaderLoader", EditorArtifactPathForHash("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"))
        });
    editorManifest.targetPlatform = "editor-windows";
    editorManifest.subAssets[0].targetPlatform = "editor-windows";
    editorManifest.dependencies.push_back({AssetDependencyKind::SourceFileHash, "Assets/Shaders/Hero.shader", "editor-source"});

    auto runtimeManifest = MakeManifest(
        shader,
        "shader:Hero/Forward#0",
        {
            MakeArtifact(shader, "shader:Hero/Forward#0", ArtifactType::Shader, "ShaderLoader", EditorArtifactPathForHash("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"))
        });
    runtimeManifest.targetPlatform = "win64";
    runtimeManifest.subAssets[0].targetPlatform = "win64";
    runtimeManifest.dependencies.push_back({AssetDependencyKind::SourceFileHash, "Assets/Shaders/Hero.shader", "runtime-source"});

    ArtifactDatabase database;
    database.UpsertManifest(editorManifest, "Assets/Shaders/Hero.shader", ArtifactRecordStatus::UpToDate);
    database.UpsertManifest(runtimeManifest, "Assets/Shaders/Hero.shader", ArtifactRecordStatus::UpToDate);

    EXPECT_EQ(database.GetIndexedSourceRecordCountForTesting(shader), 2u);
    ASSERT_NE(database.Find(shader, "shader:Hero/Forward#0", "editor-windows"), nullptr);
    ASSERT_NE(database.Find(shader, "shader:Hero/Forward#0", "win64"), nullptr);

    auto refreshedEditorManifest = editorManifest;
    refreshedEditorManifest.subAssets[0].artifactPath =
        EditorArtifactPathForHash("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    refreshedEditorManifest.dependencies[0].hashOrVersion = "editor-source-updated";
    database.UpsertManifest(refreshedEditorManifest, "Assets/Shaders/Hero.shader", ArtifactRecordStatus::UpToDate);

    EXPECT_EQ(database.GetIndexedSourceRecordCountForTesting(shader), 2u);
    const auto* editorRecord = database.Find(shader, "shader:Hero/Forward#0", "editor-windows");
    const auto* runtimeRecord = database.Find(shader, "shader:Hero/Forward#0", "win64");
    ASSERT_NE(editorRecord, nullptr);
    ASSERT_NE(runtimeRecord, nullptr);
    EXPECT_EQ(editorRecord->artifactPath, refreshedEditorManifest.subAssets[0].artifactPath);
    EXPECT_EQ(runtimeRecord->artifactPath, runtimeManifest.subAssets[0].artifactPath);

    const auto editorBuilt = database.BuildManifestForSource(shader, "editor-windows");
    ASSERT_TRUE(editorBuilt.has_value());
    ASSERT_EQ(editorBuilt->dependencies.size(), 1u);
    EXPECT_EQ(editorBuilt->dependencies[0].hashOrVersion, "editor-source-updated");

    const auto runtimeBuilt = database.BuildManifestForSource(shader, "win64");
    ASSERT_TRUE(runtimeBuilt.has_value());
    ASSERT_EQ(runtimeBuilt->dependencies.size(), 1u);
    EXPECT_EQ(runtimeBuilt->dependencies[0].hashOrVersion, "runtime-source");
}

TEST(AssetManifestTests, ArtifactDatabaseRepeatedLoadsReleaseLmdbReaders)
{
    using namespace NLS::Core::Assets;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_artifactdb_reader_release_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "Library" / "ArtifactDB";
    const auto shader = MakeAssetId("32323232-3232-4232-8232-323232323232");
    const auto manifest = MakeManifest(
        shader,
        "shader:Hero/Forward#0",
        {
            MakeArtifact(shader, "shader:Hero/Forward#0", ArtifactType::Shader, "ShaderLoader", EditorArtifactPathForHash("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"))
        });

    ArtifactDatabase writer;
    writer.UpsertManifest(manifest, "Assets/Shaders/Hero.shader", ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(writer.Save(databasePath));

    for (size_t index = 0u; index < 256u; ++index)
    {
        ArtifactDatabase reader;
        ASSERT_TRUE(reader.Load(databasePath)) << "load iteration " << index;
        EXPECT_EQ(reader.FindBySource(shader).size(), 1u);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetManifestTests, BuildManifestIncludesDependencyClosureForModelPrefab)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("22222222-2222-4222-8222-222222222222");
    const auto material = MakeAssetId("33333333-3333-4333-8333-333333333333");
    const auto texture = MakeAssetId("44444444-4444-4444-8444-444444444444");

    auto modelManifest = MakeManifest(model, "prefab:HeroScene", {
        MakeArtifact(model, "prefab:HeroScene", ArtifactType::Prefab, "prefab", RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111")),
        MakeArtifact(model, "mesh:body", ArtifactType::Mesh, "mesh", RuntimeArtifactPathForHash("2222222222222222222222222222222222222222222222222222222222222222")),
        MakeArtifact(model, "skeleton:hero", ArtifactType::Skeleton, "skeleton", RuntimeArtifactPathForHash("3333333333333333333333333333333333333333333333333333333333333333")),
        MakeArtifact(model, "skin:body", ArtifactType::Skin, "skin", RuntimeArtifactPathForHash("4444444444444444444444444444444444444444444444444444444444444444")),
        MakeArtifact(model, "animation:idle", ArtifactType::AnimationClip, "animation", RuntimeArtifactPathForHash("5555555555555555555555555555555555555555555555555555555555555555")),
        MakeArtifact(model, "morph-target:smile", ArtifactType::MorphTarget, "morph-target", RuntimeArtifactPathForHash("6666666666666666666666666666666666666666666666666666666666666666"))
    });
    modelManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, material.ToString(), "material:body"});

    auto materialManifest = MakeManifest(material, "material:body", {
        MakeArtifact(material, "material:body", ArtifactType::Material, "material", RuntimeArtifactPathForHash("7777777777777777777777777777777777777777777777777777777777777777"))
    });
    materialManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, texture.ToString(), "texture:basecolor"});

    auto textureManifest = MakeManifest(texture, "texture:basecolor", {
        MakeArtifact(texture, "texture:basecolor", ArtifactType::Texture, "texture", RuntimeArtifactPathForHash("8888888888888888888888888888888888888888888888888888888888888888"))
    });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(modelManifest);
    builder.AddArtifactManifest(materialManifest);
    builder.AddArtifactManifest(textureManifest);

    const auto result = builder.Build({{model, "prefab:HeroScene"}}, "win64");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_EQ(result.manifest.entries.size(), 8u);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "prefab:HeroScene"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "mesh:body"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "skeleton:hero"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "skin:body"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "animation:idle"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(model, "morph-target:smile"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(material, "material:body"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(texture, "texture:basecolor"), nullptr);
}

TEST(AssetManifestTests, BuildManifestIncludesNestedPrefabDependencyClosure)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto parent = MakeAssetId("66666666-6666-4666-8666-666666666666");
    const auto child = MakeAssetId("77777777-7777-4777-8777-777777777777");

    auto parentManifest = MakeManifest(parent, "prefab:Parent", {
        MakeArtifact(parent, "prefab:Parent", ArtifactType::Prefab, "prefab", RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111"))
    });
    parentManifest.dependencies.push_back({AssetDependencyKind::NestedPrefab, child.ToString(), "prefab:Child"});

    auto childManifest = MakeManifest(child, "prefab:Child", {
        MakeArtifact(child, "prefab:Child", ArtifactType::Prefab, "prefab", RuntimeArtifactPathForHash("2222222222222222222222222222222222222222222222222222222222222222"))
    });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(parentManifest);
    builder.AddArtifactManifest(childManifest);

    const auto result = builder.Build({{parent, "prefab:Parent"}}, "win64");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(parent, "prefab:Parent"), nullptr);
    EXPECT_NE(RuntimeAssetDatabase(result.manifest).Resolve(child, "prefab:Child"), nullptr);
}

TEST(AssetManifestTests, RuntimeManifestBuildSelectsArtifactManifestByTargetPlatform)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("68686868-6868-4868-8868-686868686868");

    auto winManifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111"))
    });
    winManifest.targetPlatform = "win64";
    winManifest.subAssets.front().targetPlatform = "win64";

    auto editorManifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", RuntimeArtifactPathForHash("2222222222222222222222222222222222222222222222222222222222222222"))
    });
    editorManifest.targetPlatform = "editor-windows";
    editorManifest.subAssets.front().targetPlatform = "editor-windows";

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(winManifest);
    builder.AddArtifactManifest(editorManifest);

    const auto result = builder.Build({{model, "prefab:Hero"}}, "editor-windows");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    const RuntimeAssetDatabase database(result.manifest);
    const auto* entry = database.Resolve(model, "prefab:Hero");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->artifactPath, RuntimeArtifactPathForHash("2222222222222222222222222222222222222222222222222222222222222222"));
}

TEST(AssetManifestTests, RuntimeManifestBuildRejectsMissingRootSubAsset)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("69696969-6969-4969-8969-696969696969");
    auto manifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111")),
        MakeArtifact(model, "mesh:body", ArtifactType::Mesh, "mesh", RuntimeArtifactPathForHash("2222222222222222222222222222222222222222222222222222222222222222"))
    });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(manifest);

    const auto result = builder.Build({{model, "prefab:Missing"}}, "win64");

    ASSERT_TRUE(result.diagnostics.HasErrors());
    ASSERT_FALSE(result.diagnostics.GetItems().empty());
    EXPECT_EQ(result.diagnostics.GetItems().front().code, "runtime-manifest-missing-root-subasset");
    EXPECT_TRUE(result.manifest.entries.empty());
    EXPECT_TRUE(result.manifest.prefabEntries.empty());
}

TEST(AssetManifestTests, RuntimeManifestBuildRejectsSourceOnlyArtifactPathsAndIncompleteMetadata)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("70707070-7070-4070-8070-707070707070");
    auto manifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", "Assets/Models/Hero.gltf")
    });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(manifest);

    const auto result = builder.Build({{model, "prefab:Hero"}}, "win64");

    ASSERT_TRUE(result.diagnostics.HasErrors());
    ASSERT_FALSE(result.diagnostics.GetItems().empty());
    EXPECT_EQ(result.diagnostics.GetItems().front().code, "runtime-manifest-source-artifact-path");
    EXPECT_TRUE(result.manifest.entries.empty());
}

TEST(AssetManifestTests, RuntimeManifestBuildRejectsAuthoringPathsAndEmptyArtifactMetadata)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto sourceAsset = MakeAssetId("72727272-7272-4272-8272-727272727272");
    auto authoringPathManifest = MakeManifest(sourceAsset, "material:Hero", {
        MakeArtifact(sourceAsset, "material:Hero", ArtifactType::Material, "material", "Assets/Materials/Hero.mat")
    });

    RuntimeManifestBuilder authoringPathBuilder;
    authoringPathBuilder.AddArtifactManifest(authoringPathManifest);

    const auto authoringPathResult = authoringPathBuilder.Build({{sourceAsset, "material:Hero"}}, "win64");
    ASSERT_TRUE(authoringPathResult.diagnostics.HasErrors());
    ASSERT_FALSE(authoringPathResult.diagnostics.GetItems().empty());
    EXPECT_EQ(authoringPathResult.diagnostics.GetItems().front().code, "runtime-manifest-source-artifact-path");
    EXPECT_TRUE(authoringPathResult.manifest.entries.empty());

    const auto incomplete = MakeAssetId("73737373-7373-4373-8373-737373737373");
    auto incompleteManifest = MakeManifest(incomplete, "material:Hero", {
        MakeArtifact(incomplete, "material:Hero", ArtifactType::Material, "", RuntimeArtifactPathForHash("9999999999999999999999999999999999999999999999999999999999999999"))
    });
    incompleteManifest.subAssets.front().contentHash.clear();

    RuntimeManifestBuilder incompleteBuilder;
    incompleteBuilder.AddArtifactManifest(incompleteManifest);

    const auto incompleteResult = incompleteBuilder.Build({{incomplete, "material:Hero"}}, "win64");
    ASSERT_TRUE(incompleteResult.diagnostics.HasErrors());
    ASSERT_FALSE(incompleteResult.diagnostics.GetItems().empty());
    EXPECT_EQ(incompleteResult.diagnostics.GetItems().front().code, "runtime-manifest-incomplete-artifact-metadata");
    EXPECT_TRUE(incompleteResult.manifest.entries.empty());
}

TEST(AssetManifestTests, RuntimeManifestBuildRejectsAbsoluteAndTraversalArtifactPaths)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto absoluteAsset = MakeAssetId("75757575-7575-4575-8575-757575757575");
    auto absoluteManifest = MakeManifest(absoluteAsset, "material:Hero", {
        MakeArtifact(
            absoluteAsset,
            "material:Hero",
            ArtifactType::Material,
            "material",
            (std::filesystem::temp_directory_path() / "Hero.mat").generic_string())
    });

    RuntimeManifestBuilder absoluteBuilder;
    absoluteBuilder.AddArtifactManifest(absoluteManifest);
    const auto absoluteResult = absoluteBuilder.Build({{absoluteAsset, "material:Hero"}}, "win64");
    ASSERT_TRUE(absoluteResult.diagnostics.HasErrors());
    ASSERT_FALSE(absoluteResult.diagnostics.GetItems().empty());
    EXPECT_EQ(absoluteResult.diagnostics.GetItems().front().code, "runtime-manifest-source-artifact-path");

    const auto traversalAsset = MakeAssetId("76767676-7676-4676-8676-767676767676");
    auto traversalManifest = MakeManifest(traversalAsset, "material:Hero", {
        MakeArtifact(
            traversalAsset,
            "material:Hero",
            ArtifactType::Material,
            "material",
            "../Assets/Materials/Hero.mat")
    });

    RuntimeManifestBuilder traversalBuilder;
    traversalBuilder.AddArtifactManifest(traversalManifest);
    const auto traversalResult = traversalBuilder.Build({{traversalAsset, "material:Hero"}}, "win64");
    ASSERT_TRUE(traversalResult.diagnostics.HasErrors());
    ASSERT_FALSE(traversalResult.diagnostics.GetItems().empty());
    EXPECT_EQ(traversalResult.diagnostics.GetItems().front().code, "runtime-manifest-source-artifact-path");

    const auto driveRelativeAsset = MakeAssetId("77777777-7777-4777-8777-777777777777");
    auto driveRelativeManifest = MakeManifest(driveRelativeAsset, "material:Hero", {
        MakeArtifact(
            driveRelativeAsset,
            "material:Hero",
            ArtifactType::Material,
            "material",
            "C:Artifacts/Hero.mat")
    });

    RuntimeManifestBuilder driveRelativeBuilder;
    driveRelativeBuilder.AddArtifactManifest(driveRelativeManifest);
    const auto driveRelativeResult = driveRelativeBuilder.Build({{driveRelativeAsset, "material:Hero"}}, "win64");
    ASSERT_TRUE(driveRelativeResult.diagnostics.HasErrors());
    ASSERT_FALSE(driveRelativeResult.diagnostics.GetItems().empty());
    EXPECT_EQ(driveRelativeResult.diagnostics.GetItems().front().code, "runtime-manifest-source-artifact-path");
}

TEST(AssetManifestTests, RuntimeManifestBuildIncludesGeneratedPrefabSiblingArtifacts)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("74747474-7474-4474-8474-747474747474");
    auto manifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111")),
        MakeArtifact(model, "mesh:Body", ArtifactType::Mesh, "mesh", RuntimeArtifactPathForHash("2222222222222222222222222222222222222222222222222222222222222222")),
        MakeArtifact(model, "material:Body", ArtifactType::Material, "material", RuntimeArtifactPathForHash("3333333333333333333333333333333333333333333333333333333333333333"))
    });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(manifest);

    const auto result = builder.Build({{model, "prefab:Hero"}}, "win64");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    const RuntimeAssetDatabase database(result.manifest);
    EXPECT_NE(database.Resolve(model, "prefab:Hero"), nullptr);
    EXPECT_NE(database.Resolve(model, "mesh:Body"), nullptr);
    EXPECT_NE(database.Resolve(model, "material:Body"), nullptr);
}

TEST(AssetManifestTests, RuntimeManifestBuildRejectsArtifactTargetPlatformMismatch)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;

    const auto model = MakeAssetId("71717171-7171-4171-8171-717171717171");
    auto manifest = MakeManifest(model, "prefab:Hero", {
        MakeArtifact(model, "prefab:Hero", ArtifactType::Prefab, "prefab", RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111"))
    });
    manifest.subAssets.front().targetPlatform = "editor-windows";

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(manifest);

    const auto result = builder.Build({{model, "prefab:Hero"}}, "win64");

    ASSERT_TRUE(result.diagnostics.HasErrors());
    ASSERT_FALSE(result.diagnostics.GetItems().empty());
    EXPECT_EQ(result.diagnostics.GetItems().front().code, "runtime-manifest-artifact-platform-mismatch");
    EXPECT_TRUE(result.manifest.entries.empty());
}

TEST(AssetManifestTests, RuntimeResolverRejectsSourceOnlyPaths)
{
    using namespace NLS::Engine::Assets;

    EXPECT_FALSE(IsRuntimePackagedAssetPath("Assets/Models/Hero.gltf"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Assets/Models/Hero.glb"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Assets/Models/Hero.fbx"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Assets/Models/Hero.obj"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Assets/Models/Hero.gltf.meta"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Library/SourceAssetDatabase.json"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("Artifacts/material1023"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("../Artifacts/Hero/1111111111111111111111111111111111111111111111111111111111111111"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("/Artifacts/Hero/1111111111111111111111111111111111111111111111111111111111111111"));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("../" + RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111")));
    EXPECT_FALSE(IsRuntimePackagedAssetPath("/" + RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111")));
    EXPECT_TRUE(IsRuntimePackagedAssetPath(RuntimeArtifactPathForHash("1111111111111111111111111111111111111111111111111111111111111111")));
}

TEST(AssetManifestTests, SceneMaterialBindingsRequireObjectReferenceIdentity)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Engine::Assets;
    using namespace NLS::Engine::Rendering;
    using namespace NLS::Engine::Serialize;

    const auto material = MakeAssetId("56565656-5656-4656-8656-565656565656");

    RuntimeAssetManifest manifest;
    manifest.entries.push_back({
        material,
        "material:logo",
        ArtifactType::Material,
        "material",
        RuntimeArtifactPathForHash("c60d974c2b7848cc361ca9d7562fc367637856a8de4f7a0bc5136b6015046f55"),
        "sha256:logo",
        {}
    });

    RuntimeAssetDatabase database(manifest);

    PrefabArtifact prefab;
    ObjectRecord meshRenderer;
    meshRenderer.id = ObjectId(NLS::Guid::Parse("57575757-5757-4757-8757-575757575757"));
    meshRenderer.localIdentifierInFile = MakeLocalIdentifierInFile(meshRenderer.id);
    meshRenderer.typeName = "NLS::Engine::Components::MeshRenderer";
    meshRenderer.debugName = "String MeshRenderer";
    meshRenderer.properties.push_back({"materials", PropertyValue::Array({
        PropertyValue::String("Materials/Logo.mat")
    })});
    prefab.graph.objects.push_back(std::move(meshRenderer));

    const auto bindings = ResolveSceneRendererMaterialBindings(prefab, database);
    ASSERT_EQ(bindings.size(), 1u);
    EXPECT_FALSE(bindings.front().reference.assetId.IsValid());
    EXPECT_TRUE(bindings.front().reference.subAssetKey.empty());
    EXPECT_FALSE(bindings.front().resolved);
    EXPECT_TRUE(bindings.front().artifactPath.empty());
}
