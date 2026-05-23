#include "Rendering/ShaderCompiler/ShaderCacheDatabase.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace NLS::Render::ShaderCompiler
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

size_t ParseSize(const std::string& value)
{
    size_t result = 0u;
    std::from_chars(value.data(), value.data() + value.size(), result);
    return result;
}

template<typename T>
T ParseEnum(const std::string& value)
{
    int parsed = 0;
    std::from_chars(value.data(), value.data() + value.size(), parsed);
    return static_cast<T>(parsed);
}

template<typename T>
int ToInt(const T value)
{
    return static_cast<int>(value);
}

void AppendField(std::ostream& stream, const std::string& field)
{
    stream << EscapeField(field);
}
}

void ShaderCacheDatabase::Clear()
{
    m_records.clear();
    m_indexByKey.clear();
}

void ShaderCacheDatabase::Upsert(
    const ShaderCompilationInput& input,
    const ShaderCompilationOutput& output,
    std::string sourceFingerprint,
    std::string toolchainIdentity)
{
    ShaderCacheRecord record;
    record.cacheKey = output.cacheKey;
    record.assetPath = input.assetPath;
    record.stage = input.stage;
    record.targetPlatform = input.options.targetPlatform;
    record.entryPoint = input.options.entryPoint;
    record.targetProfile = input.options.targetProfile;
    record.status = output.status;
    record.artifactPath = output.status == ShaderCompilationStatus::Succeeded ? output.artifactPath : std::string {};
    record.diagnostics = output.diagnostics;
    record.sourceFingerprint = std::move(sourceFingerprint);
    record.toolchainIdentity = std::move(toolchainIdentity);
    record.bytecodeSize = output.bytecode.size();
    record.dependencyCount = output.dependencyPaths.size();

    const auto key = MakeKey(record.cacheKey, record.stage, record.targetPlatform);
    const auto found = m_indexByKey.find(key);
    if (found != m_indexByKey.end() && found->second < m_records.size())
        m_records[found->second] = std::move(record);
    else
        m_records.push_back(std::move(record));

    RebuildIndex();
}

void ShaderCacheDatabase::RemoveByAssetPath(const std::string& assetPath)
{
    m_records.erase(
        std::remove_if(
            m_records.begin(),
            m_records.end(),
            [&assetPath](const ShaderCacheRecord& record)
            {
                return record.assetPath == assetPath;
            }),
        m_records.end());
    RebuildIndex();
}

const ShaderCacheRecord* ShaderCacheDatabase::Find(
    const std::string& cacheKey,
    const ShaderStage stage,
    const ShaderTargetPlatform targetPlatform) const
{
    const auto found = m_indexByKey.find(MakeKey(cacheKey, stage, targetPlatform));
    if (found == m_indexByKey.end() || found->second >= m_records.size())
        return nullptr;
    return &m_records[found->second];
}

ShaderCacheDatabaseStats ShaderCacheDatabase::GetStats() const
{
    ShaderCacheDatabaseStats stats;
    stats.totalRecords = m_records.size();
    for (const auto& record : m_records)
    {
        if (record.status == ShaderCompilationStatus::Succeeded)
            ++stats.succeededRecords;
        else if (record.status == ShaderCompilationStatus::Failed)
            ++stats.failedRecords;
    }
    return stats;
}

bool ShaderCacheDatabase::Save(const std::filesystem::path& path) const
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output << "Nullus.ShaderCacheDB\t1\n";
    for (const auto& record : m_records)
    {
        AppendField(output, record.cacheKey); output << '\t';
        AppendField(output, record.assetPath); output << '\t';
        output << ToInt(record.stage) << '\t';
        output << ToInt(record.targetPlatform) << '\t';
        AppendField(output, record.entryPoint); output << '\t';
        AppendField(output, record.targetProfile); output << '\t';
        output << ToInt(record.status) << '\t';
        AppendField(output, record.artifactPath); output << '\t';
        AppendField(output, record.diagnostics); output << '\t';
        AppendField(output, record.sourceFingerprint); output << '\t';
        AppendField(output, record.toolchainIdentity); output << '\t';
        output << record.bytecodeSize << '\t';
        output << record.dependencyCount << '\n';
    }
    return output.good();
}

bool ShaderCacheDatabase::Load(const std::filesystem::path& path)
{
    Clear();

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    std::string line;
    if (!std::getline(input, line) || line != "Nullus.ShaderCacheDB\t1")
        return false;

    while (std::getline(input, line))
    {
        if (line.empty())
            continue;

        const auto fields = SplitTabLine(line);
        if (fields.size() != 13u)
            return false;

        ShaderCacheRecord record;
        record.cacheKey = fields[0];
        record.assetPath = fields[1];
        record.stage = ParseEnum<ShaderStage>(fields[2]);
        record.targetPlatform = ParseEnum<ShaderTargetPlatform>(fields[3]);
        record.entryPoint = fields[4];
        record.targetProfile = fields[5];
        record.status = ParseEnum<ShaderCompilationStatus>(fields[6]);
        record.artifactPath = fields[7];
        record.diagnostics = fields[8];
        record.sourceFingerprint = fields[9];
        record.toolchainIdentity = fields[10];
        record.bytecodeSize = ParseSize(fields[11]);
        record.dependencyCount = ParseSize(fields[12]);
        m_records.push_back(std::move(record));
    }

    RebuildIndex();
    return true;
}

void ShaderCacheDatabase::RebuildIndex()
{
    m_indexByKey.clear();
    for (size_t index = 0u; index < m_records.size(); ++index)
    {
        const auto& record = m_records[index];
        m_indexByKey[MakeKey(record.cacheKey, record.stage, record.targetPlatform)] = index;
    }
}

std::string ShaderCacheDatabase::MakeKey(
    const std::string& cacheKey,
    const ShaderStage stage,
    const ShaderTargetPlatform targetPlatform)
{
    return cacheKey + "\n" +
        std::to_string(ToInt(stage)) + "\n" +
        std::to_string(ToInt(targetPlatform));
}
}
