#include "Assets/ArtifactDatabase.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace NLS::Core::Assets
{
namespace
{
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

void AppendField(std::ostream& stream, const std::string& field)
{
    stream << EscapeField(field);
}
}

void ArtifactDatabase::Clear()
{
    m_records.clear();
    m_indexByKey.clear();
#if defined(NLS_ENABLE_TEST_HOOKS)
    m_indexRebuildCountForTesting = 0u;
#endif
}

void ArtifactDatabase::UpsertManifest(
    const ArtifactManifest& manifest,
    std::string sourcePath,
    const ArtifactRecordStatus status)
{
    RemoveSourceAndUpdateIndex(manifest.sourceAssetId);

    for (const auto& artifact : manifest.subAssets)
    {
        ArtifactDatabaseRecord record;
        record.sourceAssetId = manifest.sourceAssetId;
        record.sourcePath = sourcePath;
        record.subAssetKey = artifact.subAssetKey;
        record.artifactType = artifact.artifactType;
        record.loaderId = artifact.loaderId;
        record.targetPlatform = artifact.targetPlatform.empty() ? manifest.targetPlatform : artifact.targetPlatform;
        record.artifactPath = artifact.artifactPath;
        record.contentHash = artifact.contentHash;
        record.importerId = manifest.importerId;
        record.importerVersion = manifest.importerVersion;
        record.primarySubAssetKey = manifest.primarySubAssetKey;
        record.dependencyCount = manifest.dependencies.size();
        record.status = status;
        AddRecord(std::move(record));
    }
}

void ArtifactDatabase::MarkStatus(const AssetId sourceAssetId, const ArtifactRecordStatus status)
{
    for (auto& record : m_records)
    {
        if (record.sourceAssetId == sourceAssetId)
            record.status = status;
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
    for (const auto& record : m_records)
    {
        if (record.sourceAssetId == sourceAssetId)
            result.push_back(&record);
    }
    return result;
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
#endif

bool ArtifactDatabase::Save(const std::filesystem::path& path) const
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output << "Nullus.ArtifactDB\t1\n";
    for (const auto& record : m_records)
    {
        AppendField(output, record.sourceAssetId.ToString()); output << '\t';
        AppendField(output, record.sourcePath); output << '\t';
        AppendField(output, record.subAssetKey); output << '\t';
        output << ToInt(record.artifactType) << '\t';
        AppendField(output, record.loaderId); output << '\t';
        AppendField(output, record.targetPlatform); output << '\t';
        AppendField(output, record.artifactPath); output << '\t';
        AppendField(output, record.contentHash); output << '\t';
        AppendField(output, record.importerId); output << '\t';
        output << record.importerVersion << '\t';
        AppendField(output, record.primarySubAssetKey); output << '\t';
        output << record.dependencyCount << '\t';
        output << ToInt(record.status) << '\n';
    }
    return output.good();
}

bool ArtifactDatabase::Load(const std::filesystem::path& path)
{
    Clear();

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    std::string line;
    if (!std::getline(input, line) || line != "Nullus.ArtifactDB\t1")
        return false;

    while (std::getline(input, line))
    {
        if (line.empty())
            continue;

        const auto fields = SplitTabLine(line);
        if (fields.size() != 13u)
            return false;

        ArtifactDatabaseRecord record;
        const auto guid = NLS::Guid::TryParse(fields[0]);
        if (!guid.has_value())
            return false;
        record.sourceAssetId = AssetId(*guid);
        record.sourcePath = fields[1];
        record.subAssetKey = fields[2];
        record.artifactType = ToArtifactType(fields[3]);
        record.loaderId = fields[4];
        record.targetPlatform = fields[5];
        record.artifactPath = fields[6];
        record.contentHash = fields[7];
        record.importerId = fields[8];
        record.importerVersion = ParseU32(fields[9]);
        record.primarySubAssetKey = fields[10];
        record.dependencyCount = ParseSize(fields[11]);
        record.status = ToStatus(fields[12]);
        m_records.push_back(std::move(record));
    }

    RebuildIndex();
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
}

void ArtifactDatabase::RemoveRecordAt(const size_t index)
{
    if (index >= m_records.size())
        return;

    const auto removedKey = MakeKey(
        m_records[index].sourceAssetId,
        m_records[index].subAssetKey,
        m_records[index].targetPlatform);
    m_indexByKey.erase(removedKey);

    const auto lastIndex = m_records.size() - 1u;
    if (index != lastIndex)
    {
        m_records[index] = std::move(m_records[lastIndex]);
        m_indexByKey[MakeKey(
            m_records[index].sourceAssetId,
            m_records[index].subAssetKey,
            m_records[index].targetPlatform)] = index;
    }

    m_records.pop_back();
}

void ArtifactDatabase::AddRecord(ArtifactDatabaseRecord record)
{
    const auto index = m_records.size();
    auto key = MakeKey(record.sourceAssetId, record.subAssetKey, record.targetPlatform);
    m_records.push_back(std::move(record));
    m_indexByKey[std::move(key)] = index;
}

void ArtifactDatabase::RebuildIndex()
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ++m_indexRebuildCountForTesting;
#endif
    m_indexByKey.clear();
    for (size_t index = 0u; index < m_records.size(); ++index)
    {
        const auto& record = m_records[index];
        m_indexByKey[MakeKey(record.sourceAssetId, record.subAssetKey, record.targetPlatform)] = index;
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
