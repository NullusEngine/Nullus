#include "Assets/ArtifactDatabase.h"

#include <lmdb.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace NLS::Core::Assets
{
namespace
{
constexpr const char* kArtifactDatabaseSchema = "Nullus.ArtifactDB.LMDB";
constexpr const char* kArtifactDatabaseVersion = "4";
constexpr const char* kMetaDatabaseName = "meta";
constexpr const char* kManifestDatabaseName = "manifest";
constexpr const char* kRecordsDatabaseName = "records";
constexpr const char* kDependenciesDatabaseName = "dependencies";
constexpr size_t kDefaultLmdbMapSize = 64ull * 1024ull * 1024ull;
constexpr size_t kLmdbMapGrowthPadding = 16ull * 1024ull * 1024ull;
constexpr size_t kLmdbRecordOverheadEstimate = 512u;

struct LmdbEnv
{
    MDB_env* env = nullptr;

    ~LmdbEnv()
    {
        if (env != nullptr)
            mdb_env_close(env);
    }
};

struct LmdbTxn
{
    MDB_txn* txn = nullptr;
    bool committed = false;

    ~LmdbTxn()
    {
        if (txn != nullptr && !committed)
            mdb_txn_abort(txn);
    }
};

std::string LmdbErrorMessage(const std::string& operation, const int result)
{
    return operation + " failed: " + mdb_strerror(result);
}

MDB_val ToMdbVal(const std::string& value)
{
    return MDB_val {
        value.size(),
        const_cast<char*>(value.data())
    };
}

std::string FromMdbVal(const MDB_val& value)
{
    return std::string(
        static_cast<const char*>(value.mv_data),
        static_cast<size_t>(value.mv_size));
}

bool OpenEnvironment(
    const std::filesystem::path& path,
    const bool readOnly,
    LmdbEnv& environment,
    std::string* errorMessage,
    const size_t mapSize = kDefaultLmdbMapSize)
{
    std::error_code error;
    if (readOnly)
    {
        if (!std::filesystem::is_directory(path, error))
        {
            if (errorMessage != nullptr)
                *errorMessage = "Artifact database path is not a readable directory: " + path.string();
            return false;
        }
    }
    else
    {
        if (std::filesystem::exists(path, error) && !std::filesystem::is_directory(path, error))
        {
            if (errorMessage != nullptr)
                *errorMessage = "Artifact database path exists but is not a directory: " + path.string();
            return false;
        }
        std::filesystem::create_directories(path, error);
        if (error)
        {
            if (errorMessage != nullptr)
                *errorMessage = "Failed to create artifact database directory " + path.string() + ": " + error.message();
            return false;
        }
    }

    int result = mdb_env_create(&environment.env);
    if (result != MDB_SUCCESS)
    {
        if (errorMessage != nullptr)
            *errorMessage = LmdbErrorMessage("mdb_env_create", result);
        return false;
    }

    result = mdb_env_set_maxdbs(environment.env, 8u);
    if (result != MDB_SUCCESS)
    {
        if (errorMessage != nullptr)
            *errorMessage = LmdbErrorMessage("mdb_env_set_maxdbs", result);
        return false;
    }

    result = mdb_env_set_mapsize(environment.env, std::max(mapSize, kDefaultLmdbMapSize));
    if (result != MDB_SUCCESS)
    {
        if (errorMessage != nullptr)
            *errorMessage = LmdbErrorMessage("mdb_env_set_mapsize", result);
        return false;
    }

    const auto pathString = path.string();
    const unsigned int flags = readOnly ? MDB_RDONLY : 0u;
    result = mdb_env_open(environment.env, pathString.c_str(), flags, 0664);
    if (result != MDB_SUCCESS)
    {
        if (errorMessage != nullptr)
            *errorMessage = LmdbErrorMessage("mdb_env_open", result);
        return false;
    }
    return true;
}

bool PutString(MDB_txn* txn, const MDB_dbi database, const std::string& key, const std::string& value)
{
    auto keyValue = ToMdbVal(key);
    auto dataValue = ToMdbVal(value);
    return mdb_put(txn, database, &keyValue, &dataValue, 0u) == MDB_SUCCESS;
}

std::optional<std::string> GetString(MDB_txn* txn, const MDB_dbi database, const std::string& key)
{
    auto keyValue = ToMdbVal(key);
    MDB_val dataValue {};
    const auto result = mdb_get(txn, database, &keyValue, &dataValue);
    if (result != MDB_SUCCESS)
        return std::nullopt;
    return FromMdbVal(dataValue);
}

std::string EscapeField(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '\\': escaped += "\\\\"; break;
        case '\t': escaped += "\\t"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

std::string UnescapeField(const std::string& value)
{
    std::string unescaped;
    unescaped.reserve(value.size());
    for (size_t index = 0u; index < value.size(); ++index)
    {
        if (value[index] != '\\' || index + 1u >= value.size())
        {
            unescaped.push_back(value[index]);
            continue;
        }

        const char escaped = value[++index];
        switch (escaped)
        {
        case 't': unescaped.push_back('\t'); break;
        case 'n': unescaped.push_back('\n'); break;
        case 'r': unescaped.push_back('\r'); break;
        case '\\': unescaped.push_back('\\'); break;
        default:
            unescaped.push_back('\\');
            unescaped.push_back(escaped);
            break;
        }
    }
    return unescaped;
}

std::vector<std::string> SplitTabLine(const std::string& line)
{
    std::vector<std::string> fields;
    std::string field;
    std::istringstream stream(line);
    while (std::getline(stream, field, '\t'))
        fields.push_back(UnescapeField(field));
    if (!line.empty() && line.back() == '\t')
        fields.emplace_back();
    return fields;
}

uint32_t ParseU32(const std::string& value)
{
    uint32_t result = 0u;
    std::from_chars(value.data(), value.data() + value.size(), result);
    return result;
}

size_t ParseSize(const std::string& value)
{
    size_t result = 0u;
    std::from_chars(value.data(), value.data() + value.size(), result);
    return result;
}

std::optional<size_t> TryParseSizeStrict(const std::string& value)
{
    if (value.empty())
        return std::nullopt;

    size_t result = 0u;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size())
        return std::nullopt;
    return result;
}

int ToInt(const ArtifactType value)
{
    return static_cast<int>(value);
}

ArtifactType ToArtifactType(const std::string& value)
{
    int parsed = 0;
    std::from_chars(value.data(), value.data() + value.size(), parsed);
    return static_cast<ArtifactType>(parsed);
}

int ToInt(const ArtifactRecordStatus value)
{
    return static_cast<int>(value);
}

ArtifactRecordStatus ToStatus(const std::string& value)
{
    int parsed = 0;
    std::from_chars(value.data(), value.data() + value.size(), parsed);
    return static_cast<ArtifactRecordStatus>(parsed);
}

std::vector<std::string> SplitRecordKey(const std::string& key)
{
    std::vector<std::string> fields;
    std::string field;
    std::istringstream stream(key);
    while (std::getline(stream, field, '\n'))
        fields.push_back(std::move(field));
    if (!key.empty() && key.back() == '\n')
        fields.emplace_back();
    return fields;
}

void AppendField(std::ostream& stream, const std::string& field)
{
    stream << EscapeField(field);
}

std::string EncodeRecord(const ArtifactDatabaseRecord& record)
{
    std::ostringstream output;
    AppendField(output, record.sourceAssetId.ToString()); output << '\t';
    AppendField(output, record.sourcePath); output << '\t';
    AppendField(output, record.subAssetKey); output << '\t';
    output << ToInt(record.artifactType) << '\t';
    AppendField(output, record.loaderId); output << '\t';
    AppendField(output, record.targetPlatform); output << '\t';
    AppendField(output, record.artifactPath); output << '\t';
    AppendField(output, record.contentHash); output << '\t';
    AppendField(output, record.displayName); output << '\t';
    AppendField(output, record.importerId); output << '\t';
    output << record.importerVersion << '\t';
    AppendField(output, record.primarySubAssetKey); output << '\t';
    output << record.dependencyCount << '\t';
    output << ToInt(record.status);
    return output.str();
}

std::optional<ArtifactDatabaseRecord> DecodeRecord(const std::string& value)
{
    const auto fields = SplitTabLine(value);
    if (fields.size() != 14u)
        return std::nullopt;

    const auto guid = NLS::Guid::TryParse(fields[0]);
    if (!guid.has_value())
        return std::nullopt;
    const auto dependencyCount = TryParseSizeStrict(fields[12]);
    if (!dependencyCount.has_value())
        return std::nullopt;

    ArtifactDatabaseRecord record;
    record.sourceAssetId = AssetId(*guid);
    record.sourcePath = fields[1];
    record.subAssetKey = fields[2];
    record.artifactType = ToArtifactType(fields[3]);
    record.loaderId = fields[4];
    record.targetPlatform = fields[5];
    record.artifactPath = fields[6];
    record.contentHash = fields[7];
    record.displayName = fields[8];
    record.importerId = fields[9];
    record.importerVersion = ParseU32(fields[10]);
    record.primarySubAssetKey = fields[11];
    record.dependencyCount = *dependencyCount;
    record.status = ToStatus(fields[13]);
    return record;
}

std::string EncodeManifestHeader(const ArtifactDatabase::ManifestHeader& header)
{
    std::ostringstream output;
    AppendField(output, header.sourceAssetId.ToString()); output << '\t';
    AppendField(output, header.sourcePath); output << '\t';
    AppendField(output, header.importerId); output << '\t';
    output << header.importerVersion << '\t';
    AppendField(output, header.targetPlatform); output << '\t';
    AppendField(output, header.primarySubAssetKey); output << '\t';
    output << header.dependencyCount << '\t';
    output << ToInt(header.status);
    return output.str();
}

std::optional<ArtifactDatabase::ManifestHeader> DecodeManifestHeader(const std::string& value)
{
    const auto fields = SplitTabLine(value);
    if (fields.size() != 8u)
        return std::nullopt;

    const auto guid = NLS::Guid::TryParse(fields[0]);
    if (!guid.has_value())
        return std::nullopt;
    const auto dependencyCount = TryParseSizeStrict(fields[6]);
    if (!dependencyCount.has_value())
        return std::nullopt;

    ArtifactDatabase::ManifestHeader header;
    header.sourceAssetId = AssetId(*guid);
    header.sourcePath = fields[1];
    header.importerId = fields[2];
    header.importerVersion = ParseU32(fields[3]);
    header.targetPlatform = fields[4];
    header.primarySubAssetKey = fields[5];
    header.dependencyCount = *dependencyCount;
    header.status = ToStatus(fields[7]);
    return header;
}

std::string EncodeDependency(const AssetDependencyRecord& dependency)
{
    std::ostringstream output;
    output << static_cast<int>(dependency.kind) << '\t';
    AppendField(output, dependency.value); output << '\t';
    AppendField(output, dependency.hashOrVersion);
    return output.str();
}

std::optional<AssetDependencyRecord> DecodeDependency(const std::string& value)
{
    const auto fields = SplitTabLine(value);
    if (fields.size() != 3u)
        return std::nullopt;

    AssetDependencyRecord dependency;
    dependency.kind = static_cast<AssetDependencyKind>(ParseU32(fields[0]));
    dependency.value = fields[1];
    dependency.hashOrVersion = fields[2];
    return dependency;
}

std::string MakeStorageKey(
    const AssetId sourceAssetId,
    const std::string& subAssetKey,
    const std::string& targetPlatform)
{
    return sourceAssetId.ToString() + "\n" + subAssetKey + "\n" + targetPlatform;
}

std::string RecordStorageKey(const ArtifactDatabaseRecord& record)
{
    return MakeStorageKey(record.sourceAssetId, record.subAssetKey, record.targetPlatform);
}

std::string DependencyStorageKey(const std::string& dependencyKey, const size_t index)
{
    return dependencyKey + "\n" + std::to_string(index);
}

std::string DependencyCountKey(const std::string& dependencyKey)
{
    return dependencyKey + "\ncount";
}

bool IsDependencyCountKey(const std::string& key)
{
    constexpr std::string_view suffix = "\ncount";
    return key.size() >= suffix.size() &&
        std::string_view(key).substr(key.size() - suffix.size()) == suffix;
}

std::string_view DependencyCountBaseKey(const std::string& key)
{
    constexpr std::string_view suffix = "\ncount";
    return std::string_view(key).substr(0u, key.size() - suffix.size());
}

size_t EstimateSaveMapSize(
    const std::vector<ArtifactDatabaseRecord>& records,
    const std::unordered_map<std::string, ArtifactDatabase::ManifestHeader>& manifestHeadersBySourceTarget,
    const std::unordered_map<std::string, std::vector<AssetDependencyRecord>>& dependenciesBySourceTarget)
{
    size_t bytes = kLmdbMapGrowthPadding;
    bytes += std::string(kArtifactDatabaseSchema).size();
    bytes += std::string(kArtifactDatabaseVersion).size();

    for (const auto& record : records)
    {
        bytes += RecordStorageKey(record).size();
        bytes += EncodeRecord(record).size();
        bytes += kLmdbRecordOverheadEstimate;
    }

    for (const auto& [key, header] : manifestHeadersBySourceTarget)
    {
        bytes += key.size();
        bytes += EncodeManifestHeader(header).size();
        bytes += kLmdbRecordOverheadEstimate;
    }

    for (const auto& [key, dependencies] : dependenciesBySourceTarget)
    {
        bytes += DependencyCountKey(key).size();
        bytes += std::to_string(dependencies.size()).size();
        bytes += kLmdbRecordOverheadEstimate;

        for (size_t index = 0u; index < dependencies.size(); ++index)
        {
            bytes += DependencyStorageKey(key, index).size();
            bytes += EncodeDependency(dependencies[index]).size();
            bytes += kLmdbRecordOverheadEstimate;
        }
    }

    size_t mapSize = std::max(bytes * 2u, kDefaultLmdbMapSize);
    constexpr size_t kAlignment = 64ull * 1024ull * 1024ull;
    const size_t remainder = mapSize % kAlignment;
    if (remainder != 0u)
        mapSize += kAlignment - remainder;
    return mapSize;
}
}

void ArtifactDatabase::Clear()
{
    m_records.clear();
    m_manifestHeadersBySourceTarget.clear();
    m_dependenciesBySourceTarget.clear();
    m_indexByKey.clear();
    m_indicesBySourceAssetId.clear();
    ClearLastError();
#if defined(NLS_ENABLE_TEST_HOOKS)
    m_indexRebuildCountForTesting = 0u;
#endif
}

void ArtifactDatabase::UpsertManifest(
    const ArtifactManifest& manifest,
    std::string sourcePath,
    const ArtifactRecordStatus status)
{
    RemoveSourceTargetAndUpdateIndex(manifest.sourceAssetId, manifest.targetPlatform);
    const auto sourceTargetKey = MakeKey(manifest.sourceAssetId, {}, manifest.targetPlatform);
    m_manifestHeadersBySourceTarget[sourceTargetKey] = {
        manifest.sourceAssetId,
        std::move(sourcePath),
        manifest.importerId,
        manifest.importerVersion,
        manifest.targetPlatform,
        manifest.primarySubAssetKey,
        manifest.dependencies.size(),
        status
    };
    m_dependenciesBySourceTarget[sourceTargetKey] = manifest.dependencies;

    for (const auto& artifact : manifest.subAssets)
    {
        if (!IsContentStorageArtifactPath(artifact.artifactPath))
            continue;

        ArtifactDatabaseRecord record;
        record.sourceAssetId = manifest.sourceAssetId;
        record.sourcePath = m_manifestHeadersBySourceTarget[sourceTargetKey].sourcePath;
        record.subAssetKey = artifact.subAssetKey;
        record.artifactType = artifact.artifactType;
        record.loaderId = artifact.loaderId;
        record.targetPlatform = artifact.targetPlatform.empty() ? manifest.targetPlatform : artifact.targetPlatform;
        record.artifactPath = artifact.artifactPath;
        record.contentHash = artifact.contentHash;
        record.displayName = artifact.displayName;
        record.importerId = manifest.importerId;
        record.importerVersion = manifest.importerVersion;
        record.primarySubAssetKey = manifest.primarySubAssetKey;
        record.dependencyCount = manifest.dependencies.size();
        record.status = status;
        AddRecord(std::move(record));
    }
}

std::optional<ArtifactManifest> ArtifactDatabase::BuildManifestForSource(const AssetId sourceAssetId) const
{
    for (const auto& [key, header] : m_manifestHeadersBySourceTarget)
    {
        (void)key;
        if (header.sourceAssetId == sourceAssetId)
            return BuildManifestForSource(sourceAssetId, header.targetPlatform);
    }
    return std::nullopt;
}

std::optional<ArtifactManifest> ArtifactDatabase::BuildManifestForSource(
    const AssetId sourceAssetId,
    const std::string& targetPlatform) const
{
    const auto sourceTargetKey = MakeKey(sourceAssetId, {}, targetPlatform);
    const auto header = m_manifestHeadersBySourceTarget.find(sourceTargetKey);
    if (header == m_manifestHeadersBySourceTarget.end())
        return std::nullopt;
    const auto records = FindBySource(sourceAssetId);
    std::vector<const ArtifactDatabaseRecord*> targetRecords;
    targetRecords.reserve(records.size());
    for (const auto* record : records)
    {
        if (record != nullptr && record->targetPlatform == targetPlatform)
            targetRecords.push_back(record);
    }

    ArtifactManifest manifest;
    manifest.sourceAssetId = sourceAssetId;
    manifest.importerId = header->second.importerId;
    manifest.importerVersion = header->second.importerVersion;
    manifest.targetPlatform = header->second.targetPlatform;
    manifest.primarySubAssetKey = header->second.primarySubAssetKey;
    if (const auto dependencies = m_dependenciesBySourceTarget.find(MakeKey(sourceAssetId, {}, manifest.targetPlatform));
        dependencies != m_dependenciesBySourceTarget.end())
    {
        manifest.dependencies = dependencies->second;
    }

    manifest.subAssets.reserve(targetRecords.size());
    for (const auto* record : targetRecords)
    {
        if (record == nullptr)
            continue;

        ImportedArtifact artifact;
        artifact.sourceAssetId = record->sourceAssetId;
        artifact.subAssetKey = record->subAssetKey;
        artifact.artifactType = record->artifactType;
        artifact.loaderId = record->loaderId;
        artifact.targetPlatform = record->targetPlatform;
        artifact.artifactPath = record->artifactPath;
        artifact.contentHash = record->contentHash;
        artifact.displayName = record->displayName;
        manifest.subAssets.push_back(std::move(artifact));
    }
    return manifest;
}

void ArtifactDatabase::MarkStatus(const AssetId sourceAssetId, const ArtifactRecordStatus status)
{
    for (auto& record : m_records)
    {
        if (record.sourceAssetId == sourceAssetId)
            record.status = status;
    }
    for (auto& [key, header] : m_manifestHeadersBySourceTarget)
    {
        (void)key;
        if (header.sourceAssetId == sourceAssetId)
            header.status = status;
    }
}

void ArtifactDatabase::RemoveSource(const AssetId sourceAssetId)
{
    RemoveSourceAndUpdateIndex(sourceAssetId);
}

const ArtifactDatabaseRecord* ArtifactDatabase::Find(
    const AssetId sourceAssetId,
    const std::string& subAssetKey,
    const std::string& targetPlatform) const
{
    const auto found = m_indexByKey.find(MakeKey(sourceAssetId, subAssetKey, targetPlatform));
    if (found == m_indexByKey.end() || found->second >= m_records.size())
        return nullptr;
    return &m_records[found->second];
}

std::vector<const ArtifactDatabaseRecord*> ArtifactDatabase::FindBySource(const AssetId sourceAssetId) const
{
    std::vector<const ArtifactDatabaseRecord*> result;
    const auto found = m_indicesBySourceAssetId.find(sourceAssetId);
    if (found == m_indicesBySourceAssetId.end())
        return result;

    result.reserve(found->second.size());
    for (const auto index : found->second)
    {
        if (index < m_records.size() && m_records[index].sourceAssetId == sourceAssetId)
            result.push_back(&m_records[index]);
    }
    return result;
}

const std::vector<ArtifactDatabaseRecord>& ArtifactDatabase::GetRecords() const
{
    return m_records;
}

void ArtifactDatabase::VisitRecords(const std::function<void(const ArtifactDatabaseRecord&)>& visitor) const
{
    if (!visitor)
        return;

    for (const auto& record : m_records)
        visitor(record);
}

ArtifactDatabaseStats ArtifactDatabase::GetStats() const
{
    ArtifactDatabaseStats stats;
    stats.totalRecords = m_records.size();
    for (const auto& record : m_records)
    {
        switch (record.status)
        {
        case ArtifactRecordStatus::Importing: ++stats.importingRecords; break;
        case ArtifactRecordStatus::UpToDate: ++stats.upToDateRecords; break;
        case ArtifactRecordStatus::Stale: ++stats.staleRecords; break;
        case ArtifactRecordStatus::Failed: ++stats.failedRecords; break;
        case ArtifactRecordStatus::Unknown:
        default:
            break;
        }
    }
    return stats;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
size_t ArtifactDatabase::GetIndexRebuildCountForTesting() const
{
    return m_indexRebuildCountForTesting;
}

size_t ArtifactDatabase::GetIndexedSourceRecordCountForTesting(const AssetId sourceAssetId) const
{
    const auto found = m_indicesBySourceAssetId.find(sourceAssetId);
    return found != m_indicesBySourceAssetId.end() ? found->second.size() : 0u;
}

bool ArtifactDatabase::MutateRecordForTesting(
    const AssetId sourceAssetId,
    const std::string& subAssetKey,
    const std::string& targetPlatform,
    const std::function<void(ArtifactDatabaseRecord&)>& mutator)
{
    if (!mutator)
        return false;

    const auto found = m_indexByKey.find(MakeKey(sourceAssetId, subAssetKey, targetPlatform));
    if (found == m_indexByKey.end() || found->second >= m_records.size())
        return false;

    mutator(m_records[found->second]);
    RebuildIndex();
    return true;
}

size_t ArtifactDatabase::MutateRecordsForTesting(
    const std::function<bool(ArtifactDatabaseRecord&)>& mutator)
{
    if (!mutator)
        return 0u;

    size_t mutated = 0u;
    for (auto& record : m_records)
    {
        if (mutator(record))
            ++mutated;
    }

    if (mutated > 0u)
        RebuildIndex();
    return mutated;
}
#endif

std::string ArtifactDatabase::GetLastError() const
{
    return m_lastError;
}

void ArtifactDatabase::ClearLastError() const
{
    m_lastError.clear();
}

bool ArtifactDatabase::Fail(std::string message) const
{
    m_lastError = std::move(message);
    return false;
}

bool ArtifactDatabase::FailLmdb(std::string operation, const int result) const
{
    return Fail(LmdbErrorMessage(operation, result));
}

bool ArtifactDatabase::Save(const std::filesystem::path& path) const
{
    ClearLastError();

    LmdbEnv environment;
    std::string errorMessage;
    if (!OpenEnvironment(
            path,
            false,
            environment,
            &errorMessage,
            EstimateSaveMapSize(m_records, m_manifestHeadersBySourceTarget, m_dependenciesBySourceTarget)))
    {
        return Fail(errorMessage);
    }

    int result = MDB_SUCCESS;
    LmdbTxn transaction;
    result = mdb_txn_begin(environment.env, nullptr, 0u, &transaction.txn);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_txn_begin", result);

    MDB_dbi metaDatabase = 0u;
    MDB_dbi manifestDatabase = 0u;
    MDB_dbi recordsDatabase = 0u;
    MDB_dbi dependenciesDatabase = 0u;
    result = mdb_dbi_open(transaction.txn, kMetaDatabaseName, MDB_CREATE, &metaDatabase);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_dbi_open(meta)", result);
    result = mdb_dbi_open(transaction.txn, kManifestDatabaseName, MDB_CREATE, &manifestDatabase);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_dbi_open(manifest)", result);
    result = mdb_dbi_open(transaction.txn, kRecordsDatabaseName, MDB_CREATE, &recordsDatabase);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_dbi_open(records)", result);
    result = mdb_dbi_open(transaction.txn, kDependenciesDatabaseName, MDB_CREATE, &dependenciesDatabase);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_dbi_open(dependencies)", result);

    result = mdb_drop(transaction.txn, metaDatabase, 0);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_drop(meta)", result);
    result = mdb_drop(transaction.txn, manifestDatabase, 0);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_drop(manifest)", result);
    result = mdb_drop(transaction.txn, recordsDatabase, 0);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_drop(records)", result);
    result = mdb_drop(transaction.txn, dependenciesDatabase, 0);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_drop(dependencies)", result);

    if (!PutString(transaction.txn, metaDatabase, "schema", kArtifactDatabaseSchema) ||
        !PutString(transaction.txn, metaDatabase, "version", kArtifactDatabaseVersion))
    {
        return Fail("Failed to write artifact database metadata.");
    }

    for (const auto& [key, header] : m_manifestHeadersBySourceTarget)
    {
        if (!PutString(transaction.txn, manifestDatabase, key, EncodeManifestHeader(header)))
            return Fail("Failed to write artifact manifest header.");
    }

    for (const auto& record : m_records)
    {
        if (!PutString(transaction.txn, recordsDatabase, RecordStorageKey(record), EncodeRecord(record)))
            return Fail("Failed to write artifact database record.");
    }

    for (const auto& [key, dependencies] : m_dependenciesBySourceTarget)
    {
        if (!PutString(transaction.txn, dependenciesDatabase, DependencyCountKey(key), std::to_string(dependencies.size())))
            return Fail("Failed to write artifact dependency count.");

        for (size_t index = 0u; index < dependencies.size(); ++index)
        {
            if (!PutString(transaction.txn, dependenciesDatabase, DependencyStorageKey(key, index), EncodeDependency(dependencies[index])))
                return Fail("Failed to write artifact dependency record.");
        }
    }

    result = mdb_txn_commit(transaction.txn);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_txn_commit", result);
    transaction.committed = true;
    return true;
}

bool ArtifactDatabase::Load(const std::filesystem::path& path)
{
    Clear();
    ArtifactDatabase loaded;

    LmdbEnv environment;
    std::string errorMessage;
    if (!OpenEnvironment(path, true, environment, &errorMessage))
        return Fail(errorMessage);

    int result = MDB_SUCCESS;
    LmdbTxn transaction;
    result = mdb_txn_begin(environment.env, nullptr, MDB_RDONLY, &transaction.txn);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_txn_begin", result);

    MDB_dbi metaDatabase = 0u;
    MDB_dbi manifestDatabase = 0u;
    MDB_dbi recordsDatabase = 0u;
    MDB_dbi dependenciesDatabase = 0u;
    result = mdb_dbi_open(transaction.txn, kMetaDatabaseName, 0u, &metaDatabase);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_dbi_open(meta)", result);
    result = mdb_dbi_open(transaction.txn, kManifestDatabaseName, 0u, &manifestDatabase);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_dbi_open(manifest)", result);
    result = mdb_dbi_open(transaction.txn, kRecordsDatabaseName, 0u, &recordsDatabase);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_dbi_open(records)", result);
    result = mdb_dbi_open(transaction.txn, kDependenciesDatabaseName, 0u, &dependenciesDatabase);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_dbi_open(dependencies)", result);

    if (GetString(transaction.txn, metaDatabase, "schema").value_or(std::string {}) != kArtifactDatabaseSchema ||
        GetString(transaction.txn, metaDatabase, "version").value_or(std::string {}) != kArtifactDatabaseVersion)
    {
        return Fail("Artifact database schema or version does not match the supported LMDB format.");
    }

    MDB_cursor* manifestCursor = nullptr;
    result = mdb_cursor_open(transaction.txn, manifestDatabase, &manifestCursor);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_cursor_open(manifest)", result);

    MDB_val key {};
    MDB_val value {};
    auto cursorResult = mdb_cursor_get(manifestCursor, &key, &value, MDB_FIRST);
    for (;
        cursorResult == MDB_SUCCESS;
        cursorResult = mdb_cursor_get(manifestCursor, &key, &value, MDB_NEXT))
    {
        const auto storageKey = FromMdbVal(key);
        auto header = DecodeManifestHeader(FromMdbVal(value));
        if (!header.has_value())
        {
            mdb_cursor_close(manifestCursor);
            return Fail("Artifact manifest header payload could not be decoded.");
        }
        if (storageKey != MakeKey(header->sourceAssetId, {}, header->targetPlatform))
        {
            mdb_cursor_close(manifestCursor);
            return Fail("Artifact manifest header key does not match its encoded identity.");
        }
        loaded.m_manifestHeadersBySourceTarget.emplace(storageKey, std::move(*header));
    }
    mdb_cursor_close(manifestCursor);
    if (cursorResult != MDB_NOTFOUND)
        return FailLmdb("mdb_cursor_get(manifest)", cursorResult);

    MDB_cursor* recordsCursor = nullptr;
    result = mdb_cursor_open(transaction.txn, recordsDatabase, &recordsCursor);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_cursor_open(records)", result);

    cursorResult = mdb_cursor_get(recordsCursor, &key, &value, MDB_FIRST);
    for (;
        cursorResult == MDB_SUCCESS;
        cursorResult = mdb_cursor_get(recordsCursor, &key, &value, MDB_NEXT))
    {
        const auto storageKey = FromMdbVal(key);
        auto record = DecodeRecord(FromMdbVal(value));
        if (!record.has_value())
        {
            mdb_cursor_close(recordsCursor);
            return Fail("Artifact database record payload could not be decoded.");
        }
        if (storageKey != RecordStorageKey(*record))
        {
            mdb_cursor_close(recordsCursor);
            return Fail("Artifact database record storage key does not match its encoded record identity.");
        }
        if (!IsContentStorageArtifactPath(record->artifactPath))
        {
            return Fail("Artifact database record artifact path must use content-addressed storage.");
        }
        loaded.m_records.push_back(std::move(*record));
    }
    mdb_cursor_close(recordsCursor);
    if (cursorResult != MDB_NOTFOUND)
        return FailLmdb("mdb_cursor_get(records)", cursorResult);

    MDB_cursor* dependenciesCursor = nullptr;
    result = mdb_cursor_open(transaction.txn, dependenciesDatabase, &dependenciesCursor);
    if (result != MDB_SUCCESS)
        return FailLmdb("mdb_cursor_open(dependencies)", result);

    struct PendingDependency
    {
        size_t index = 0u;
        AssetDependencyRecord record;
    };

    std::unordered_map<std::string, size_t> dependencyCounts;
    std::unordered_map<std::string, std::vector<PendingDependency>> pendingDependencies;

    cursorResult = mdb_cursor_get(dependenciesCursor, &key, &value, MDB_FIRST);
    for (;
        cursorResult == MDB_SUCCESS;
        cursorResult = mdb_cursor_get(dependenciesCursor, &key, &value, MDB_NEXT))
    {
        const auto dependencyKey = FromMdbVal(key);
        if (IsDependencyCountKey(dependencyKey))
        {
            auto count = TryParseSizeStrict(FromMdbVal(value));
            if (!count.has_value())
            {
                mdb_cursor_close(dependenciesCursor);
                return Fail("Artifact dependency count could not be decoded.");
            }

            dependencyCounts.emplace(std::string(DependencyCountBaseKey(dependencyKey)), *count);
            continue;
        }

        const auto split = dependencyKey.rfind('\n');
        if (split == std::string::npos || split + 1u >= dependencyKey.size())
        {
            mdb_cursor_close(dependenciesCursor);
            return Fail("Artifact dependency key is malformed.");
        }

        auto dependencyIndex = TryParseSizeStrict(dependencyKey.substr(split + 1u));
        if (!dependencyIndex.has_value())
        {
            mdb_cursor_close(dependenciesCursor);
            return Fail("Artifact dependency index could not be decoded.");
        }

        auto dependency = DecodeDependency(FromMdbVal(value));
        if (!dependency.has_value())
        {
            mdb_cursor_close(dependenciesCursor);
            return Fail("Artifact dependency payload could not be decoded.");
        }

        pendingDependencies[dependencyKey.substr(0u, split)].push_back({*dependencyIndex, std::move(*dependency)});
    }
    mdb_cursor_close(dependenciesCursor);
    if (cursorResult != MDB_NOTFOUND)
        return FailLmdb("mdb_cursor_get(dependencies)", cursorResult);

    for (const auto& [dependencyKey, entries] : pendingDependencies)
    {
        if (dependencyCounts.find(dependencyKey) == dependencyCounts.end())
            return Fail("Artifact dependency records are missing their count entry.");
    }

    for (const auto& [dependencyKey, dependencyCount] : dependencyCounts)
    {
        const auto header = loaded.m_manifestHeadersBySourceTarget.find(dependencyKey);
        if (header == loaded.m_manifestHeadersBySourceTarget.end())
            return Fail("Artifact dependency records are missing their manifest header.");
        if (header->second.dependencyCount != dependencyCount)
            return Fail("Artifact manifest header dependency count does not match stored dependency records.");

        std::vector<std::optional<AssetDependencyRecord>> orderedDependencies(dependencyCount);
        if (const auto entries = pendingDependencies.find(dependencyKey); entries != pendingDependencies.end())
        {
            for (const auto& entry : entries->second)
            {
                if (entry.index >= dependencyCount || orderedDependencies[entry.index].has_value())
                    return Fail("Artifact dependency indices are out of range or duplicated.");
                orderedDependencies[entry.index] = entry.record;
            }
        }

        auto& dependencies = loaded.m_dependenciesBySourceTarget[dependencyKey];
        dependencies.reserve(dependencyCount);
        for (auto& dependency : orderedDependencies)
        {
            if (!dependency.has_value())
                return Fail("Artifact dependency indices are not contiguous.");
            dependencies.push_back(std::move(*dependency));
        }
    }

    for (const auto& record : loaded.m_records)
    {
        const auto dependencyKey = MakeKey(record.sourceAssetId, {}, record.targetPlatform);
        const auto header = loaded.m_manifestHeadersBySourceTarget.find(dependencyKey);
        if (header == loaded.m_manifestHeadersBySourceTarget.end())
            return Fail("Artifact record is missing its manifest header.");
        const auto dependencies = loaded.m_dependenciesBySourceTarget.find(dependencyKey);
        if (dependencyCounts.find(dependencyKey) == dependencyCounts.end())
            return Fail("Artifact record is missing its dependency count entry.");
        const auto dependencyCount =
            dependencies != loaded.m_dependenciesBySourceTarget.end() ? dependencies->second.size() : 0u;
        if (dependencyCount != record.dependencyCount)
            return Fail("Artifact record dependency count does not match stored dependency records.");
    }

    loaded.RebuildIndex();
    m_records = std::move(loaded.m_records);
    m_manifestHeadersBySourceTarget = std::move(loaded.m_manifestHeadersBySourceTarget);
    m_dependenciesBySourceTarget = std::move(loaded.m_dependenciesBySourceTarget);
    m_indexByKey = std::move(loaded.m_indexByKey);
    m_indicesBySourceAssetId = std::move(loaded.m_indicesBySourceAssetId);
#if defined(NLS_ENABLE_TEST_HOOKS)
    m_indexRebuildCountForTesting = loaded.m_indexRebuildCountForTesting;
#endif
    mdb_txn_abort(transaction.txn);
    transaction.txn = nullptr;
    ClearLastError();
    return true;
}

void ArtifactDatabase::RemoveSourceAndUpdateIndex(const AssetId sourceAssetId)
{
    for (size_t index = 0u; index < m_records.size();)
    {
        if (m_records[index].sourceAssetId == sourceAssetId)
        {
            RemoveRecordAt(index);
            continue;
        }

        ++index;
    }
    for (auto it = m_dependenciesBySourceTarget.begin(); it != m_dependenciesBySourceTarget.end();)
    {
        const auto fields = SplitRecordKey(it->first);
        const auto matches = fields.size() == 3u && fields[0] == sourceAssetId.ToString();
        if (matches)
            it = m_dependenciesBySourceTarget.erase(it);
        else
            ++it;
    }
    for (auto it = m_manifestHeadersBySourceTarget.begin(); it != m_manifestHeadersBySourceTarget.end();)
    {
        const auto fields = SplitRecordKey(it->first);
        const auto matches = fields.size() == 3u && fields[0] == sourceAssetId.ToString();
        if (matches)
            it = m_manifestHeadersBySourceTarget.erase(it);
        else
            ++it;
    }
}

void ArtifactDatabase::RemoveSourceTargetAndUpdateIndex(
    const AssetId sourceAssetId,
    const std::string& targetPlatform)
{
    for (size_t index = 0u; index < m_records.size();)
    {
        if (m_records[index].sourceAssetId == sourceAssetId &&
            m_records[index].targetPlatform == targetPlatform)
        {
            RemoveRecordAt(index);
            continue;
        }

        ++index;
    }

    m_dependenciesBySourceTarget.erase(MakeKey(sourceAssetId, {}, targetPlatform));
    m_manifestHeadersBySourceTarget.erase(MakeKey(sourceAssetId, {}, targetPlatform));
}

void ArtifactDatabase::RemoveRecordAt(const size_t index)
{
    if (index >= m_records.size())
        return;

    const auto removedKey = MakeKey(
        m_records[index].sourceAssetId,
        m_records[index].subAssetKey,
        m_records[index].targetPlatform);
    const auto removedSourceAssetId = m_records[index].sourceAssetId;
    m_indexByKey.erase(removedKey);

    auto removeSourceIndex = [this](const AssetId sourceAssetId, const size_t recordIndex)
    {
        const auto found = m_indicesBySourceAssetId.find(sourceAssetId);
        if (found == m_indicesBySourceAssetId.end())
            return;

        auto& indices = found->second;
        const auto indexIt = std::find(indices.begin(), indices.end(), recordIndex);
        if (indexIt != indices.end())
            indices.erase(indexIt);
        if (indices.empty())
            m_indicesBySourceAssetId.erase(found);
    };

    const auto lastIndex = m_records.size() - 1u;
    removeSourceIndex(removedSourceAssetId, index);
    if (index != lastIndex)
    {
        const auto movedSourceAssetId = m_records[lastIndex].sourceAssetId;
        m_records[index] = std::move(m_records[lastIndex]);
        m_indexByKey[MakeKey(
            m_records[index].sourceAssetId,
            m_records[index].subAssetKey,
            m_records[index].targetPlatform)] = index;
        auto& movedIndices = m_indicesBySourceAssetId[movedSourceAssetId];
        const auto movedIndex = std::find(movedIndices.begin(), movedIndices.end(), lastIndex);
        if (movedIndex != movedIndices.end())
            *movedIndex = index;
        else
            movedIndices.push_back(index);
    }

    m_records.pop_back();
}

void ArtifactDatabase::AddRecord(ArtifactDatabaseRecord record)
{
    const auto index = m_records.size();
    auto key = MakeKey(record.sourceAssetId, record.subAssetKey, record.targetPlatform);
    const auto sourceAssetId = record.sourceAssetId;
    m_records.push_back(std::move(record));
    m_indexByKey[std::move(key)] = index;
    m_indicesBySourceAssetId[sourceAssetId].push_back(index);
}

void ArtifactDatabase::RebuildIndex()
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ++m_indexRebuildCountForTesting;
#endif
    m_indexByKey.clear();
    m_indicesBySourceAssetId.clear();
    for (size_t index = 0u; index < m_records.size(); ++index)
    {
        const auto& record = m_records[index];
        m_indexByKey[MakeKey(record.sourceAssetId, record.subAssetKey, record.targetPlatform)] = index;
        m_indicesBySourceAssetId[record.sourceAssetId].push_back(index);
    }
}

std::string ArtifactDatabase::MakeKey(
    const AssetId sourceAssetId,
    const std::string& subAssetKey,
    const std::string& targetPlatform)
{
    return sourceAssetId.ToString() + "\n" + subAssetKey + "\n" + targetPlatform;
}
}
