#include "Assets/NpakArchive.h"

#include "Assets/ArtifactManifest.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <system_error>

namespace NLS::Core::Assets
{
namespace
{
constexpr uint32_t kMagic = 0x4B41504Eu; // "NPAK" little-endian
constexpr uint16_t kFormatVersion = 1u;
constexpr uint16_t kHeaderSize = 128u;
constexpr uint64_t kFileRecordSize = 80u;
constexpr uint64_t kBlockRecordSize = 24u;
constexpr uint32_t kFileFlagHashed = 1u;
constexpr uint32_t kMinBlockSize = 4u * 1024u;
constexpr uint32_t kMaxBlockSize = 64u * 1024u * 1024u;

constexpr uint32_t kCodecStore = static_cast<uint32_t>(NpakCodec::Store);
constexpr uint32_t kCodecZstd = static_cast<uint32_t>(NpakCodec::Zstandard);

constexpr uint32_t kFnvOffset = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

uint32_t FnvUpdate(uint32_t state, const uint8_t* bytes, const size_t byteCount)
{
    for (size_t index = 0u; index < byteCount; ++index)
    {
        state ^= bytes[index];
        state *= kFnvPrime;
    }
    return state;
}

uint32_t FnvUpdate(uint32_t state, std::span<const uint8_t> bytes)
{
    return FnvUpdate(state, bytes.data(), bytes.size());
}

void AppendU16(std::vector<uint8_t>& bytes, const uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
}

void AppendU32(std::vector<uint8_t>& bytes, const uint32_t value)
{
    for (uint32_t shift = 0u; shift < 32u; shift += 8u)
        bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
}

void AppendU64(std::vector<uint8_t>& bytes, const uint64_t value)
{
    for (uint32_t shift = 0u; shift < 64u; shift += 8u)
        bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
}

uint16_t ReadU16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8u);
}

uint32_t ReadU32(const uint8_t* bytes)
{
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < 4u; ++index)
        value |= static_cast<uint32_t>(bytes[index]) << (index * 8u);
    return value;
}

uint64_t ReadU64(const uint8_t* bytes)
{
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
        value |= static_cast<uint64_t>(bytes[index]) << (index * 8u);
    return value;
}

struct HeaderFields
{
    uint32_t flags = 0u;
    uint32_t codecId = kCodecStore;
    uint32_t codecVersion = 0u;
    uint32_t blockSize = 0u;
    uint32_t blockCount = 0u;
    uint64_t fileCount = 0u;
    uint64_t fileIndexOffset = 0u;
    uint64_t fileIndexSize = 0u;
    uint64_t blockIndexOffset = 0u;
    uint64_t blockIndexSize = 0u;
    uint64_t stringTableOffset = 0u;
    uint64_t stringTableSize = 0u;
    uint64_t totalSize = 0u;
    uint32_t headerChecksum = 0u;
    uint32_t indexChecksum = 0u;
};

std::vector<uint8_t> EncodeHeader(const HeaderFields& fields)
{
    std::vector<uint8_t> header(static_cast<size_t>(kHeaderSize), 0u);
    header[0] = static_cast<uint8_t>(kMagic & 0xFFu);
    header[1] = static_cast<uint8_t>((kMagic >> 8u) & 0xFFu);
    header[2] = static_cast<uint8_t>((kMagic >> 16u) & 0xFFu);
    header[3] = static_cast<uint8_t>((kMagic >> 24u) & 0xFFu);
    header[4] = static_cast<uint8_t>(kFormatVersion & 0xFFu);
    header[5] = static_cast<uint8_t>((kFormatVersion >> 8u) & 0xFFu);
    header[6] = static_cast<uint8_t>(kHeaderSize & 0xFFu);
    header[7] = static_cast<uint8_t>((kHeaderSize >> 8u) & 0xFFu);
    auto writeU32 = [&header](const size_t offset, const uint32_t value)
    {
        for (uint32_t index = 0u; index < 4u; ++index)
            header[offset + index] = static_cast<uint8_t>((value >> (index * 8u)) & 0xFFu);
    };
    auto writeU64 = [&header](const size_t offset, const uint64_t value)
    {
        for (uint32_t index = 0u; index < 8u; ++index)
            header[offset + index] = static_cast<uint8_t>((value >> (index * 8u)) & 0xFFu);
    };
    writeU32(8u, fields.flags);
    writeU32(12u, fields.codecId);
    writeU32(16u, fields.codecVersion);
    writeU32(20u, fields.blockSize);
    writeU32(24u, fields.blockCount);
    writeU64(32u, fields.fileCount);
    writeU64(40u, fields.fileIndexOffset);
    writeU64(48u, fields.fileIndexSize);
    writeU64(56u, fields.blockIndexOffset);
    writeU64(64u, fields.blockIndexSize);
    writeU64(72u, fields.stringTableOffset);
    writeU64(80u, fields.stringTableSize);
    writeU64(88u, fields.totalSize);
    writeU32(116u, fields.indexChecksum);
    writeU32(112u, FnvUpdate(kFnvOffset, header.data(), 112u));
    return header;
}

bool DecodeHeader(const uint8_t* bytes, const size_t byteCount, HeaderFields& fields, std::string* diagnostics)
{
    const auto fail = [diagnostics](std::string message)
    {
        if (diagnostics != nullptr)
            *diagnostics = std::move(message);
        return false;
    };

    if (byteCount < kHeaderSize)
        return fail("npak: file is smaller than the versioned header");
    if (ReadU32(bytes) != kMagic)
        return fail("npak: bad magic (not a Nullus package)");
    const auto version = ReadU16(bytes + 4u);
    if (version != kFormatVersion)
        return fail("npak: unsupported format version " + std::to_string(version));
    if (ReadU16(bytes + 6u) != kHeaderSize)
        return fail("npak: incompatible header size");

    fields.flags = ReadU32(bytes + 8u);
    fields.codecId = ReadU32(bytes + 12u);
    fields.codecVersion = ReadU32(bytes + 16u);
    fields.blockSize = ReadU32(bytes + 20u);
    fields.blockCount = ReadU32(bytes + 24u);
    fields.fileCount = ReadU64(bytes + 32u);
    fields.fileIndexOffset = ReadU64(bytes + 40u);
    fields.fileIndexSize = ReadU64(bytes + 48u);
    fields.blockIndexOffset = ReadU64(bytes + 56u);
    fields.blockIndexSize = ReadU64(bytes + 64u);
    fields.stringTableOffset = ReadU64(bytes + 72u);
    fields.stringTableSize = ReadU64(bytes + 80u);
    fields.totalSize = ReadU64(bytes + 88u);
    fields.headerChecksum = ReadU32(bytes + 112u);
    fields.indexChecksum = ReadU32(bytes + 116u);

    if (FnvUpdate(kFnvOffset, bytes, 112u) != fields.headerChecksum)
        return fail("npak: header checksum mismatch");

    if (fields.blockSize < kMinBlockSize || fields.blockSize > kMaxBlockSize)
        return fail("npak: block size out of range");
    if (fields.codecId != kCodecStore && fields.codecId != kCodecZstd)
        return fail("npak: unknown block codec");
    if (fields.blockCount == 0u && fields.fileCount != 0u)
        return fail("npak: file index without block index");
    if (fields.fileCount > (std::numeric_limits<uint64_t>::max)() / kFileRecordSize)
        return fail("npak: file count overflow");
    if (fields.blockCount > (std::numeric_limits<uint64_t>::max)() / kBlockRecordSize)
        return fail("npak: block count overflow");
    if (fields.fileIndexSize != fields.fileCount * kFileRecordSize)
        return fail("npak: file index size mismatch");
    if (fields.blockIndexSize != fields.blockCount * kBlockRecordSize)
        return fail("npak: block index size mismatch");

    const auto withinFile = [&fields](const uint64_t offset, const uint64_t size)
    {
        return offset >= kHeaderSize && offset <= fields.totalSize &&
            size <= fields.totalSize - offset;
    };
    if (!withinFile(fields.fileIndexOffset, fields.fileIndexSize) ||
        !withinFile(fields.blockIndexOffset, fields.blockIndexSize) ||
        !withinFile(fields.stringTableOffset, fields.stringTableSize))
    {
        return fail("npak: index region out of bounds");
    }

    return true;
}

std::string PathKey(const std::string& path)
{
    return path;
}
} // namespace

uint32_t NpakChecksum(const uint8_t* bytes, const size_t byteCount)
{
    return FnvUpdate(kFnvOffset, bytes, byteCount);
}

uint32_t NpakChecksum(const std::span<const uint8_t> bytes)
{
    return NpakChecksum(bytes.data(), bytes.size());
}

const char* NpakCodecName(const uint32_t codecId)
{
    switch (codecId)
    {
    case kCodecStore: return "store";
    case kCodecZstd: return "zstd";
    default: return "unknown";
    }
}

bool IsValidNpakPath(const std::string& path)
{
    if (path.empty() || path.size() > 1024u)
        return false;
    if (path.front() == '/' || path.front() == '\\')
        return false;
    if (path.find('\\') != std::string::npos)
        return false;
    if (path.find(':') != std::string::npos)
        return false;
    if (path.back() == '/')
        return false;

    size_t start = 0u;
    while (start <= path.size())
    {
        const auto next = path.find('/', start);
        const auto length = (next == std::string::npos ? path.size() : next) - start;
        const auto segment = path.substr(start, length);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        for (const auto character : segment)
        {
            if (static_cast<unsigned char>(character) < 0x20u)
                return false;
        }
        if (next == std::string::npos)
            break;
        start = next + 1u;
    }
    return true;
}

// ---------------------------------------------------------------------------
// NpakArchive
// ---------------------------------------------------------------------------

NpakArchive::NpakArchive(NpakArchive&& other) noexcept
{
    *this = std::move(other);
}

NpakArchive& NpakArchive::operator=(NpakArchive&& other) noexcept
{
    if (this == &other) return *this;
    // Mutexes belong to their object. Transfer the protected archive and cache
    // while preventing concurrent reads from either handle.
    std::scoped_lock lock(m_ioMutex,m_cacheMutex,other.m_ioMutex,other.m_cacheMutex);
    m_path = std::move(other.m_path);
    m_info = std::move(other.m_info);
    m_entries = std::move(other.m_entries);
    m_entryIndex = std::move(other.m_entryIndex);
    m_blocks = std::move(other.m_blocks);
    m_stream = std::move(other.m_stream);
    m_cache = std::move(other.m_cache);
    m_cacheStamp = std::move(other.m_cacheStamp);
    m_cacheClock = other.m_cacheClock;
    m_cacheStats = other.m_cacheStats;
    m_cacheBudgetBytes = other.m_cacheBudgetBytes;
    other.m_cacheClock = 0;
    other.m_cacheStats = {};
    return *this;
}

std::optional<NpakArchive> NpakArchive::Open(const std::filesystem::path& path, std::string* diagnostics)
{
    NpakArchive archive;
    if (!archive.Load(path, diagnostics))
        return std::nullopt;
    return std::optional<NpakArchive>(std::move(archive));
}

bool NpakArchive::Load(const std::filesystem::path& path, std::string* diagnostics)
{
    const auto fail = [diagnostics](const std::string& message)
    {
        if (diagnostics != nullptr)
            *diagnostics = message;
        return false;
    };

    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize < kHeaderSize)
        return fail("npak: cannot open package '" + path.string() + "'");

    m_stream.open(path, std::ios::binary);
    if (!m_stream)
        return fail("npak: cannot read package '" + path.string() + "'");

    std::vector<uint8_t> header(static_cast<size_t>(kHeaderSize), 0u);
    m_stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (m_stream.gcount() != static_cast<std::streamsize>(header.size()))
        return fail("npak: truncated header");

    HeaderFields fields;
    if (!DecodeHeader(header.data(), header.size(), fields, diagnostics))
        return false;
    if (fields.totalSize != fileSize)
        return fail("npak: total size mismatch (truncated or extended package)");

    std::vector<uint8_t> indexBytes(
        static_cast<size_t>(fields.fileIndexSize + fields.blockIndexSize + fields.stringTableSize),
        0u);
    m_stream.seekg(static_cast<std::streamoff>(fields.fileIndexOffset), std::ios::beg);
    m_stream.read(
        reinterpret_cast<char*>(indexBytes.data()),
        static_cast<std::streamsize>(fields.fileIndexSize));
    m_stream.seekg(static_cast<std::streamoff>(fields.blockIndexOffset), std::ios::beg);
    m_stream.read(
        reinterpret_cast<char*>(indexBytes.data() + fields.fileIndexSize),
        static_cast<std::streamsize>(fields.blockIndexSize));
    m_stream.seekg(static_cast<std::streamoff>(fields.stringTableOffset), std::ios::beg);
    m_stream.read(
        reinterpret_cast<char*>(indexBytes.data() + fields.fileIndexSize + fields.blockIndexSize),
        static_cast<std::streamsize>(fields.stringTableSize));
    if (!m_stream)
        return fail("npak: truncated index");

    if (FnvUpdate(kFnvOffset, indexBytes.data(), indexBytes.size()) != fields.indexChecksum)
        return fail("npak: index checksum mismatch");

    const auto* fileRecords = indexBytes.data();
    const auto* blockRecords = indexBytes.data() + fields.fileIndexSize;
    const auto* stringTable = indexBytes.data() + fields.fileIndexSize + fields.blockIndexSize;

    m_path = path;
    m_info.formatVersion = kFormatVersion;
    m_info.codecId = fields.codecId;
    m_info.codecVersion = fields.codecVersion;
    m_info.blockSize = fields.blockSize;
    m_info.fileCount = fields.fileCount;
    m_info.blockCount = fields.blockCount;
    m_info.totalSize = fields.totalSize;
    m_info.codecName = NpakCodecName(fields.codecId);

    m_blocks.reserve(static_cast<size_t>(fields.blockCount));
    for (uint64_t index = 0u; index < fields.blockCount; ++index)
    {
        const auto* record = blockRecords + index * kBlockRecordSize;
        BlockRecord block;
        block.offset = ReadU64(record);
        block.storedSize = ReadU32(record + 8u);
        block.rawSize = ReadU32(record + 12u);
        block.codec = ReadU32(record + 16u);
        block.checksum = ReadU32(record + 20u);
        const auto codecValid = block.codec == kCodecStore || block.codec == kCodecZstd;
        if (!codecValid ||
            block.rawSize == 0u ||
            block.rawSize > fields.blockSize ||
            block.storedSize == 0u ||
            block.offset < kHeaderSize ||
            block.offset + block.storedSize > fields.totalSize)
        {
            return fail("npak: block record out of bounds");
        }
        m_info.storedBytes += block.storedSize;
        m_info.rawBytes += block.rawSize;
        m_blocks.push_back(block);
    }

    const auto stringTableBytes = static_cast<size_t>(fields.stringTableSize);
    m_entries.reserve(static_cast<size_t>(fields.fileCount));
    for (uint64_t index = 0u; index < fields.fileCount; ++index)
    {
        const auto* record = fileRecords + index * kFileRecordSize;
        NpakEntry entry;
        const auto pathOffset = ReadU64(record);
        const auto pathLength = ReadU32(record + 8u);
        const auto flags = ReadU32(record + 12u);
        entry.size = ReadU64(record + 16u);
        entry.firstBlock = ReadU64(record + 24u);
        entry.blockCount = ReadU32(record + 32u);
        entry.dataOffset = ReadU64(record + 40u);
        std::memcpy(entry.contentHash.data(), record + 48u, entry.contentHash.size());

        if ((flags & ~kFileFlagHashed) != 0u)
            return fail("npak: unknown file flags");
        if (pathLength == 0u ||
            pathOffset > stringTableBytes ||
            pathLength > stringTableBytes - pathOffset)
        {
            return fail("npak: file path out of range");
        }
        entry.path.assign(
            reinterpret_cast<const char*>(stringTable + pathOffset),
            static_cast<size_t>(pathLength));
        if (!IsValidNpakPath(entry.path))
            return fail("npak: invalid file path '" + entry.path + "'");
        if (!m_entryIndex.emplace(PathKey(entry.path), m_entries.size()).second)
            return fail("npak: duplicate file path '" + entry.path + "'");

        const uint64_t expectedBlocks = entry.size == 0u
            ? 0u
            : (entry.size + fields.blockSize - 1u) / fields.blockSize;
        if (entry.blockCount != expectedBlocks)
            return fail("npak: block count mismatch for '" + entry.path + "'");
        if (entry.firstBlock > fields.blockCount ||
            entry.blockCount > fields.blockCount - entry.firstBlock)
        {
            return fail("npak: block range out of bounds for '" + entry.path + "'");
        }
        if (entry.blockCount > 0u)
        {
            const auto& firstBlock = m_blocks[static_cast<size_t>(entry.firstBlock)];
            if (entry.dataOffset != firstBlock.offset)
                return fail("npak: data offset mismatch for '" + entry.path + "'");
        }
        m_entries.push_back(std::move(entry));
    }

    return true;
}

bool NpakArchive::Exists(const std::string_view path) const
{
    return Find(path) != nullptr;
}

const NpakEntry* NpakArchive::Find(const std::string_view path) const
{
    const auto found = m_entryIndex.find(std::string(path));
    if (found == m_entryIndex.end())
        return nullptr;
    return &m_entries[found->second];
}

std::optional<uint64_t> NpakArchive::Size(const std::string_view path) const
{
    const auto* entry = Find(path);
    if (entry == nullptr)
        return std::nullopt;
    return entry->size;
}

std::shared_ptr<const std::vector<uint8_t>> NpakArchive::TryGetCached(const uint64_t blockIndex) const
{
    std::lock_guard lock(m_cacheMutex);
    const auto found = m_cache.find(blockIndex);
    if (found == m_cache.end())
    {
        ++m_cacheStats.misses;
        return nullptr;
    }
    ++m_cacheStats.hits;
    m_cacheStamp[blockIndex] = ++m_cacheClock;
    return found->second;
}

bool NpakArchive::DecodeBlock(
    const uint64_t blockIndex,
    std::shared_ptr<const std::vector<uint8_t>>& decodedStorage) const
{
    if (auto cached = TryGetCached(blockIndex))
    {
        decodedStorage = std::move(cached);
        return true;
    }

    const auto& block = m_blocks[static_cast<size_t>(blockIndex)];
    std::vector<uint8_t> stored(block.storedSize);
    {
        std::lock_guard lock(m_ioMutex);
        m_stream.clear();
        m_stream.seekg(static_cast<std::streamoff>(block.offset), std::ios::beg);
        m_stream.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
        if (m_stream.gcount() != static_cast<std::streamsize>(stored.size()))
            return false;
    }
    if (NpakChecksum(stored.data(), stored.size()) != block.checksum)
        return false;

    std::vector<uint8_t> decoded;
    if (block.codec == kCodecStore)
    {
        if (block.storedSize != block.rawSize)
            return false;
        decoded = std::move(stored);
    }
    else
    {
        decoded.resize(block.rawSize);
        const auto produced = ZSTD_decompress(
            decoded.data(),
            decoded.size(),
            stored.data(),
            stored.size());
        if (ZSTD_isError(produced) != 0u || produced != block.rawSize)
            return false;
    }

    {
        std::lock_guard lock(m_cacheMutex);
        ++m_cacheStats.decodedBlocks;
        const auto blockSize = decoded.size();
        if (blockSize <= m_cacheBudgetBytes)
        {
            auto shared = std::make_shared<const std::vector<uint8_t>>(std::move(decoded));
            const auto existing = m_cache.find(blockIndex);
            if (existing != m_cache.end() && existing->second != nullptr)
                m_cacheStats.cachedBytes -= existing->second->size();
            m_cacheStats.cachedBytes += shared->size();
            m_cache[blockIndex] = shared;
            m_cacheStamp[blockIndex] = ++m_cacheClock;
            while (m_cacheStats.cachedBytes > m_cacheBudgetBytes && !m_cache.empty())
            {
                auto oldest = m_cache.begin();
                for (auto candidate = m_cache.begin(); candidate != m_cache.end(); ++candidate)
                {
                    if (m_cacheStamp[candidate->first] < m_cacheStamp[oldest->first])
                        oldest = candidate;
                }
                m_cacheStats.cachedBytes -= oldest->second != nullptr ? oldest->second->size() : 0u;
                m_cacheStamp.erase(oldest->first);
                m_cache.erase(oldest);
                ++m_cacheStats.evictions;
            }
            decodedStorage = std::move(shared);
            return true;
        }
    }

    decodedStorage = std::make_shared<const std::vector<uint8_t>>(std::move(decoded));
    return true;
}

bool NpakArchive::ReadAt(
    const std::string_view path,
    const uint64_t offset,
    void* destination,
    const size_t byteCount,
    std::string* diagnostics) const
{
    const auto* entry = Find(path);
    if (entry == nullptr)
    {
        if (diagnostics != nullptr)
            *diagnostics = "npak: file not found '" + std::string(path) + "'";
        return false;
    }
    return ReadAt(*entry, offset, destination, byteCount, diagnostics);
}

bool NpakArchive::ReadAt(
    const NpakEntry& entry,
    const uint64_t offset,
    void* destination,
    const size_t byteCount,
    std::string* diagnostics) const
{
    const auto fail = [diagnostics](const std::string& message)
    {
        if (diagnostics != nullptr)
            *diagnostics = message;
        return false;
    };

    if (byteCount == 0u)
        return true;
    if (destination == nullptr)
        return fail("npak: null read destination");
    if (offset > entry.size || byteCount > entry.size - offset)
        return fail("npak: read out of range for '" + entry.path + "'");

    auto* output = static_cast<uint8_t*>(destination);
    uint64_t remaining = byteCount;
    uint64_t current = offset;
    while (remaining > 0u)
    {
        const auto blockIndex = entry.firstBlock + current / m_info.blockSize;
        const auto blockOffset = current % m_info.blockSize;
        const auto& block = m_blocks[static_cast<size_t>(blockIndex)];
        if (blockOffset >= block.rawSize)
            return fail("npak: block offset out of range for '" + entry.path + "'");

        const auto available = static_cast<uint64_t>(block.rawSize) - blockOffset;
        const auto chunk = static_cast<size_t>((std::min)(available, remaining));

        std::shared_ptr<const std::vector<uint8_t>> decoded;
        if (!DecodeBlock(blockIndex, decoded) || decoded == nullptr)
            return fail("npak: corrupt block in '" + entry.path + "'");
        if (decoded->size() < blockOffset + chunk)
            return fail("npak: decoded block too small for '" + entry.path + "'");

        std::memcpy(output, decoded->data() + blockOffset, chunk);
        output += chunk;
        current += chunk;
        remaining -= chunk;
    }
    return true;
}

bool NpakArchive::ReadAll(
    const std::string_view path,
    std::vector<uint8_t>& destination,
    std::string* diagnostics) const
{
    const auto* entry = Find(path);
    if (entry == nullptr)
    {
        if (diagnostics != nullptr)
            *diagnostics = "npak: file not found '" + std::string(path) + "'";
        return false;
    }
    destination.resize(static_cast<size_t>(entry->size));
    if (entry->size == 0u)
        return true;
    return ReadAt(*entry, 0u, destination.data(), destination.size(), diagnostics);
}

bool NpakArchive::VerifyPayload(const std::string_view path, std::string* diagnostics) const
{
    const auto* entry = Find(path);
    if (entry == nullptr)
    {
        if (diagnostics != nullptr)
            *diagnostics = "npak: file not found '" + std::string(path) + "'";
        return false;
    }

    Sha256Hasher hasher;
    const auto bufferSize = static_cast<size_t>(
        (std::min<uint64_t>)(entry->size, 4u * 1024u * 1024u));
    std::vector<uint8_t> buffer(bufferSize == 0u ? 1u : bufferSize);
    uint64_t offset = 0u;
    while (offset < entry->size)
    {
        const auto chunk = static_cast<size_t>((std::min<uint64_t>)(buffer.size(), entry->size - offset));
        if (!ReadAt(*entry, offset, buffer.data(), chunk, diagnostics))
            return false;
        if (!hasher.Update(buffer.data(), chunk))
        {
            if (diagnostics != nullptr)
                *diagnostics = "npak: hashing failed";
            return false;
        }
        offset += chunk;
    }
    const auto digest = hasher.Finalize();
    if (digest != entry->contentHash)
    {
        if (diagnostics != nullptr)
            *diagnostics = "npak: content hash mismatch for '" + entry->path + "'";
        return false;
    }
    return true;
}

void NpakArchive::SetBlockCacheBudget(const uint64_t bytes)
{
    std::lock_guard lock(m_cacheMutex);
    m_cacheBudgetBytes = bytes;
}

void NpakArchive::ClearBlockCache()
{
    std::lock_guard lock(m_cacheMutex);
    m_cache.clear();
    m_cacheStamp.clear();
    m_cacheStats.cachedBytes = 0u;
}

NpakCacheStats NpakArchive::GetCacheStats() const
{
    std::lock_guard lock(m_cacheMutex);
    return m_cacheStats;
}

// ---------------------------------------------------------------------------
// NpakWriter
// ---------------------------------------------------------------------------

namespace
{
struct PreparedEntry
{
    std::string path;
    uint64_t size = 0u;
    std::array<uint8_t, 32u> hash {};
    std::string hashKey;
    size_t sourceIndex = 0u;
    uint64_t canonicalIndex = 0u;
    bool canonical = false;
};

bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return false;
    const auto end = stream.tellg();
    if (end < 0)
        return false;
    bytes.resize(static_cast<size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty())
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good() || stream.eof();
}
} // namespace

bool NpakWriter::AddFile(
    std::string archivePath,
    const std::span<const uint8_t> bytes,
    std::string* diagnostics)
{
    if (!IsValidNpakPath(archivePath))
    {
        if (diagnostics != nullptr)
            *diagnostics = "npak: invalid archive path '" + archivePath + "'";
        return false;
    }
    for (const auto& entry : m_entries)
    {
        if (entry.path == archivePath)
        {
            if (diagnostics != nullptr)
                *diagnostics = "npak: duplicate archive path '" + archivePath + "'";
            return false;
        }
    }
    SourceEntry entry;
    entry.path = std::move(archivePath);
    entry.bytes.assign(bytes.begin(), bytes.end());
    m_entries.push_back(std::move(entry));
    return true;
}

bool NpakWriter::AddFileFromDisk(
    std::string archivePath,
    const std::filesystem::path& source,
    std::string* diagnostics)
{
    if (!IsValidNpakPath(archivePath))
    {
        if (diagnostics != nullptr)
            *diagnostics = "npak: invalid archive path '" + archivePath + "'";
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error))
    {
        if (diagnostics != nullptr)
            *diagnostics = "npak: source file missing '" + source.string() + "'";
        return false;
    }
    for (const auto& entry : m_entries)
    {
        if (entry.path == archivePath)
        {
            if (diagnostics != nullptr)
                *diagnostics = "npak: duplicate archive path '" + archivePath + "'";
            return false;
        }
    }
    SourceEntry entry;
    entry.path = std::move(archivePath);
    entry.sourceFile = source;
    entry.fromDisk = true;
    m_entries.push_back(std::move(entry));
    return true;
}

size_t NpakWriter::GetStoredFileCount() const
{
    std::vector<std::string> keys;
    keys.reserve(m_entries.size());
    for (const auto& entry : m_entries)
    {
        std::vector<uint8_t> bytes;
        Sha256Hasher hasher;
        if (entry.fromDisk)
        {
            std::ifstream stream(entry.sourceFile, std::ios::binary);
            if (!stream)
                continue;
            std::vector<uint8_t> buffer(256u * 1024u);
            while (stream)
            {
                stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                const auto read = stream.gcount();
                if (read > 0)
                    hasher.Update(buffer.data(), static_cast<size_t>(read));
            }
        }
        else
        {
            hasher.Update(entry.bytes.data(), entry.bytes.size());
        }
        const auto digest = hasher.Finalize();
        keys.push_back(ToHexString(digest));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys.size();
}

size_t NpakWriter::GetDeduplicatedFileCount() const
{
    const auto added = m_entries.size();
    const auto stored = GetStoredFileCount();
    return added > stored ? added - stored : 0u;
}

void NpakWriter::Clear()
{
    m_entries.clear();
}

bool NpakWriter::Write(
    const std::filesystem::path& destination,
    const NpakWriteOptions& options,
    std::string* diagnostics)
{
    const auto fail = [diagnostics](const std::string& message)
    {
        if (diagnostics != nullptr)
            *diagnostics = message;
        return false;
    };

    if (options.blockSize < kMinBlockSize || options.blockSize > kMaxBlockSize)
        return fail("npak: block size out of range");
    if (m_entries.empty())
        return fail("npak: cannot write an empty package");

    for (const auto& entry : m_entries)
    {
        if (!IsValidNpakPath(entry.path))
            return fail("npak: invalid archive path '" + entry.path + "'");
    }

    std::vector<PreparedEntry> prepared(m_entries.size());
    for (size_t index = 0u; index < m_entries.size(); ++index)
    {
        const auto& source = m_entries[index];
        std::vector<uint8_t> whole;
        std::span<const uint8_t> bytes;
        if (source.fromDisk)
        {
            if (!ReadWholeFile(source.sourceFile, whole))
                return fail("npak: cannot read source '" + source.sourceFile.string() + "'");
            bytes = whole;
        }
        else
        {
            bytes = source.bytes;
        }

        Sha256Hasher hasher;
        if (!hasher.Update(bytes.data(), bytes.size()))
            return fail("npak: hashing failed for '" + source.path + "'");
        auto& target = prepared[index];
        target.path = source.path;
        target.size = bytes.size();
        target.hash = hasher.Finalize();
        target.hashKey = ToHexString(target.hash) + ":" + std::to_string(target.size);
        target.sourceIndex = index;
    }

    std::sort(
        prepared.begin(),
        prepared.end(),
        [](const PreparedEntry& lhs, const PreparedEntry& rhs)
        {
            return lhs.path < rhs.path;
        });

    std::unordered_map<std::string, size_t> canonicalByHash;
    for (size_t index = 0u; index < prepared.size(); ++index)
    {
        const auto inserted = canonicalByHash.emplace(prepared[index].hashKey, index);
        if (inserted.second)
        {
            prepared[index].canonical = true;
            prepared[index].canonicalIndex = index;
        }
        else
        {
            prepared[index].canonical = false;
            prepared[index].canonicalIndex = inserted.first->second;
        }
    }

    const auto destinationParent = destination.parent_path();
    std::error_code directoryError;
    if (!destinationParent.empty())
        std::filesystem::create_directories(destinationParent, directoryError);
    const auto temporaryPath = destination.string() + ".tmp";
    std::filesystem::remove(temporaryPath, directoryError);

    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return fail("npak: cannot create '" + temporaryPath + "'");

    const auto placeholder = EncodeHeader({});
    output.write(reinterpret_cast<const char*>(placeholder.data()), static_cast<std::streamsize>(placeholder.size()));

    struct WrittenBlock
    {
        uint64_t offset = 0u;
        uint32_t storedSize = 0u;
        uint32_t rawSize = 0u;
        uint32_t codec = 0u;
        uint32_t checksum = 0u;
    };

    std::vector<WrittenBlock> writtenBlocks;
    std::vector<uint64_t> canonicalFirstBlock(prepared.size(), 0u);
    std::vector<uint32_t> canonicalBlockCount(prepared.size(), 0u);

    std::vector<uint8_t> compressionBuffer(ZSTD_compressBound(options.blockSize));
    for (size_t index = 0u; index < prepared.size(); ++index)
    {
        if (!prepared[index].canonical)
            continue;

        const auto& source = m_entries[prepared[index].sourceIndex];
        std::vector<uint8_t> whole;
        std::span<const uint8_t> bytes;
        if (source.fromDisk)
        {
            if (!ReadWholeFile(source.sourceFile, whole))
                return fail("npak: cannot re-read source '" + source.sourceFile.string() + "'");
            bytes = whole;
        }
        else
        {
            bytes = source.bytes;
        }

        canonicalFirstBlock[index] = writtenBlocks.size();
        uint64_t offset = 0u;
        while (offset < bytes.size())
        {
            const auto rawSize = static_cast<uint32_t>(
                (std::min<uint64_t>)(options.blockSize, bytes.size() - offset));
            const auto* rawData = bytes.data() + offset;

            const uint8_t* storedData = rawData;
            uint32_t storedSize = rawSize;
            uint32_t codec = kCodecStore;

            if (options.enableCompression && options.compressionLevel > 0)
            {
                const auto bound = ZSTD_compressBound(rawSize);
                if (compressionBuffer.size() < bound)
                    compressionBuffer.resize(bound);
                const auto produced = ZSTD_compress(
                    compressionBuffer.data(),
                    compressionBuffer.size(),
                    rawData,
                    rawSize,
                    options.compressionLevel);
                if (ZSTD_isError(produced) == 0u && produced < rawSize)
                {
                    storedData = compressionBuffer.data();
                    storedSize = static_cast<uint32_t>(produced);
                    codec = kCodecZstd;
                }
            }

            const auto blockOffset = static_cast<uint64_t>(output.tellp());
            output.write(reinterpret_cast<const char*>(storedData), static_cast<std::streamsize>(storedSize));
            if (!output)
                return fail("npak: failed while writing blocks");

            WrittenBlock block;
            block.offset = blockOffset;
            block.storedSize = storedSize;
            block.rawSize = rawSize;
            block.codec = codec;
            block.checksum = NpakChecksum(storedData, storedSize);
            writtenBlocks.push_back(block);
            offset += rawSize;
        }
        canonicalBlockCount[index] = static_cast<uint32_t>(writtenBlocks.size() - canonicalFirstBlock[index]);
    }

    uint64_t indexChecksum = kFnvOffset;
    std::vector<uint8_t> stringTable;
    stringTable.reserve(prepared.size() * 64u);
    std::vector<uint8_t> fileIndex(prepared.size() * static_cast<size_t>(kFileRecordSize), 0u);    for (size_t index = 0u; index < prepared.size(); ++index)
    {
        auto& entry = prepared[index];
        auto* record = fileIndex.data() + index * kFileRecordSize;
        const auto pathOffset = static_cast<uint64_t>(stringTable.size());
        stringTable.insert(stringTable.end(), entry.path.begin(), entry.path.end());
        auto writeU32 = [record](const size_t at, const uint32_t value)
        {
            for (uint32_t shift = 0u; shift < 4u; ++shift)
                record[at + shift] = static_cast<uint8_t>((value >> (shift * 8u)) & 0xFFu);
        };
        auto writeU64 = [record](const size_t at, const uint64_t value)
        {
            for (uint32_t shift = 0u; shift < 8u; ++shift)
                record[at + shift] = static_cast<uint8_t>((value >> (shift * 8u)) & 0xFFu);
        };
        writeU64(0u, pathOffset);
        writeU32(8u, static_cast<uint32_t>(entry.path.size()));
        writeU32(12u, kFileFlagHashed);
        writeU64(16u, entry.size);
        const auto canonical = entry.canonicalIndex;
        writeU64(24u, canonicalFirstBlock[canonical]);
        writeU32(32u, canonicalBlockCount[canonical]);
        writeU64(40u, canonicalFirstBlock[canonical] < writtenBlocks.size()
                ? writtenBlocks[static_cast<size_t>(canonicalFirstBlock[canonical])].offset
                : 0u);
        std::memcpy(record + 48u, entry.hash.data(), entry.hash.size());
    }

    std::vector<uint8_t> blockIndex(writtenBlocks.size() * static_cast<size_t>(kBlockRecordSize), 0u);
    for (size_t index = 0u; index < writtenBlocks.size(); ++index)
    {
        const auto& block = writtenBlocks[index];
        auto* record = blockIndex.data() + index * kBlockRecordSize;
        auto writeU32 = [record](const size_t at, const uint32_t value)
        {
            for (uint32_t shift = 0u; shift < 4u; ++shift)
                record[at + shift] = static_cast<uint8_t>((value >> (shift * 8u)) & 0xFFu);
        };
        auto writeU64 = [record](const size_t at, const uint64_t value)
        {
            for (uint32_t shift = 0u; shift < 8u; ++shift)
                record[at + shift] = static_cast<uint8_t>((value >> (shift * 8u)) & 0xFFu);
        };
        writeU64(0u, block.offset);
        writeU32(8u, block.storedSize);
        writeU32(12u, block.rawSize);
        writeU32(16u, block.codec);
        writeU32(20u, block.checksum);
    }
    // Checksum order must match the reader's contiguous index buffer:
    // [file index][block index][string table].
    indexChecksum = FnvUpdate(indexChecksum, fileIndex);
    indexChecksum = FnvUpdate(indexChecksum, blockIndex);
    indexChecksum = FnvUpdate(indexChecksum, stringTable);

    const auto stringTableOffset = static_cast<uint64_t>(output.tellp());
    output.write(reinterpret_cast<const char*>(stringTable.data()), static_cast<std::streamsize>(stringTable.size()));
    const auto fileIndexOffset = static_cast<uint64_t>(output.tellp());
    output.write(reinterpret_cast<const char*>(fileIndex.data()), static_cast<std::streamsize>(fileIndex.size()));
    const auto blockIndexOffset = static_cast<uint64_t>(output.tellp());
    output.write(reinterpret_cast<const char*>(blockIndex.data()), static_cast<std::streamsize>(blockIndex.size()));
    if (!output)
        return fail("npak: failed while writing the index");

    HeaderFields header;
    header.flags = 0u;
    header.codecId = options.enableCompression ? kCodecZstd : kCodecStore;
    header.codecVersion = static_cast<uint32_t>(ZSTD_versionNumber());
    header.blockSize = options.blockSize;
    header.blockCount = static_cast<uint32_t>(writtenBlocks.size());
    header.fileCount = prepared.size();
    header.stringTableOffset = stringTableOffset;
    header.stringTableSize = stringTable.size();
    header.fileIndexOffset = fileIndexOffset;
    header.fileIndexSize = fileIndex.size();
    header.blockIndexOffset = blockIndexOffset;
    header.blockIndexSize = blockIndex.size();
    header.totalSize = static_cast<uint64_t>(output.tellp());
    header.indexChecksum = indexChecksum;
    const auto encodedHeader = EncodeHeader(header);
    output.seekp(0, std::ios::beg);
    output.write(reinterpret_cast<const char*>(encodedHeader.data()), static_cast<std::streamsize>(encodedHeader.size()));
    output.flush();
    if (!output)
        return fail("npak: failed while finalising the header");
    output.close();

    std::error_code renameError;
    std::filesystem::remove(destination, renameError);
    std::filesystem::rename(temporaryPath, destination, renameError);
    if (renameError)
    {
        std::filesystem::remove(temporaryPath, renameError);
        return fail("npak: cannot commit '" + destination.string() + "'");
    }
    return true;
}
} // namespace NLS::Core::Assets
