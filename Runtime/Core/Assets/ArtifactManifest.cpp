#include "Assets/ArtifactManifest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace NLS::Core::Assets
{
namespace
{
std::string NormalizePortableArtifactPath(const std::string& artifactPath)
{
    return std::filesystem::path(artifactPath).lexically_normal().generic_string();
}

std::mutex g_runtimeArtifactAuthorizationMutex;
std::unordered_set<std::string> g_runtimeAuthorizedArtifactPaths;
bool g_runtimeArtifactAuthorizationEnabled = false;

uint32_t RotateRight(const uint32_t value, const uint32_t bits)
{
    return (value >> bits) | (value << (32u - bits));
}

constexpr std::array<uint32_t, 64u> kSha256RoundConstants {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#if defined(_WIN32)
BCRYPT_ALG_HANDLE GetWindowsSha256Algorithm()
{
    static const BCRYPT_ALG_HANDLE algorithm = []()
    {
        BCRYPT_ALG_HANDLE handle = nullptr;
        return BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0u))
            ? handle
            : nullptr;
    }();
    return algorithm;
}
#endif

void Sha256CompressBlocks(
    std::array<uint32_t, 8u>& state,
    const uint8_t* blocks,
    const size_t blockCount)
{
    for (size_t blockIndex = 0u; blockIndex < blockCount; ++blockIndex)
    {
        const uint8_t* block = blocks + blockIndex * 64u;
        std::array<uint32_t, 64u> words {};
        for (size_t index = 0u; index < 16u; ++index)
        {
            const size_t base = index * 4u;
            words[index] =
                (static_cast<uint32_t>(block[base]) << 24u) |
                (static_cast<uint32_t>(block[base + 1u]) << 16u) |
                (static_cast<uint32_t>(block[base + 2u]) << 8u) |
                static_cast<uint32_t>(block[base + 3u]);
        }
        for (size_t index = 16u; index < 64u; ++index)
        {
            const uint32_t s0 = RotateRight(words[index - 15u], 7u) ^
                RotateRight(words[index - 15u], 18u) ^ (words[index - 15u] >> 3u);
            const uint32_t s1 = RotateRight(words[index - 2u], 17u) ^
                RotateRight(words[index - 2u], 19u) ^ (words[index - 2u] >> 10u);
            words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
        }

        uint32_t a = state[0];
        uint32_t b = state[1];
        uint32_t c = state[2];
        uint32_t d = state[3];
        uint32_t e = state[4];
        uint32_t f = state[5];
        uint32_t g = state[6];
        uint32_t h = state[7];
        for (size_t index = 0u; index < 64u; ++index)
        {
            const uint32_t s1 = RotateRight(e, 6u) ^ RotateRight(e, 11u) ^ RotateRight(e, 25u);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + ch + kSha256RoundConstants[index] + words[index];
            const uint32_t s0 = RotateRight(a, 2u) ^ RotateRight(a, 13u) ^ RotateRight(a, 22u);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
}

class Sha256Hasher
{
public:
    Sha256Hasher()
    {
#if defined(_WIN32)
        const auto algorithm = GetWindowsSha256Algorithm();
        if (algorithm != nullptr)
        {
            if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &m_windowsHash, nullptr, 0u, nullptr, 0u, 0u)))
                m_windowsHash = nullptr;
        }
#endif
    }

    ~Sha256Hasher()
    {
#if defined(_WIN32)
        if (m_windowsHash != nullptr)
            BCryptDestroyHash(m_windowsHash);
#endif
    }

    bool Update(const uint8_t* bytes, size_t byteCount)
    {
        if (bytes == nullptr && byteCount > 0u)
            return false;

#if defined(_WIN32)
        if (m_windowsHash != nullptr)
        {
            while (byteCount > 0u)
            {
                const auto chunkByteCount = static_cast<ULONG>(std::min<size_t>(
                    byteCount,
                    (std::numeric_limits<ULONG>::max)()));
                if (!BCRYPT_SUCCESS(BCryptHashData(
                        m_windowsHash,
                        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes)),
                        chunkByteCount,
                        0u)))
                {
                    return false;
                }
                bytes += chunkByteCount;
                byteCount -= chunkByteCount;
            }
            return true;
        }
#endif

        m_byteCount += byteCount;
        if (m_bufferedByteCount > 0u)
        {
            const auto copiedByteCount = std::min(byteCount, m_buffer.size() - m_bufferedByteCount);
            if (copiedByteCount > 0u)
            {
                std::memcpy(m_buffer.data() + m_bufferedByteCount, bytes, copiedByteCount);
                m_bufferedByteCount += copiedByteCount;
                bytes += copiedByteCount;
                byteCount -= copiedByteCount;
            }
            if (m_bufferedByteCount == m_buffer.size())
            {
                Sha256CompressBlocks(m_state, m_buffer.data(), 1u);
                m_bufferedByteCount = 0u;
            }
        }

        const auto fullBlockCount = byteCount / m_buffer.size();
        if (fullBlockCount > 0u)
        {
            Sha256CompressBlocks(m_state, bytes, fullBlockCount);
            const auto fullBlockByteCount = fullBlockCount * m_buffer.size();
            bytes += fullBlockByteCount;
            byteCount -= fullBlockByteCount;
        }
        if (byteCount > 0u)
        {
            std::memcpy(m_buffer.data(), bytes, byteCount);
            m_bufferedByteCount = byteCount;
        }
        return true;
    }

    std::array<uint8_t, 32u> Finalize()
    {
#if defined(_WIN32)
        if (m_windowsHash != nullptr)
        {
            std::array<uint8_t, 32u> digest {};
            const auto succeeded = BCRYPT_SUCCESS(BCryptFinishHash(
                m_windowsHash,
                digest.data(),
                static_cast<ULONG>(digest.size()),
                0u));
            BCryptDestroyHash(m_windowsHash);
            m_windowsHash = nullptr;
            return succeeded ? digest : std::array<uint8_t, 32u> {};
        }
#endif

        auto state = m_state;
        std::array<uint8_t, 128u> tail {};
        if (m_bufferedByteCount > 0u)
            std::memcpy(tail.data(), m_buffer.data(), m_bufferedByteCount);
        tail[m_bufferedByteCount] = 0x80u;

        const size_t tailBlockCount = m_bufferedByteCount + 1u + 8u <= 64u ? 1u : 2u;
        const uint64_t bitCount = m_byteCount * 8ull;
        const size_t lengthOffset = tailBlockCount * 64u - 8u;
        for (size_t index = 0u; index < 8u; ++index)
            tail[lengthOffset + index] =
                static_cast<uint8_t>((bitCount >> ((7u - index) * 8u)) & 0xffu);
        Sha256CompressBlocks(state, tail.data(), tailBlockCount);

        std::array<uint8_t, 32u> digest {};
        for (size_t index = 0u; index < state.size(); ++index)
        {
            digest[index * 4u] = static_cast<uint8_t>((state[index] >> 24u) & 0xffu);
            digest[index * 4u + 1u] = static_cast<uint8_t>((state[index] >> 16u) & 0xffu);
            digest[index * 4u + 2u] = static_cast<uint8_t>((state[index] >> 8u) & 0xffu);
            digest[index * 4u + 3u] = static_cast<uint8_t>(state[index] & 0xffu);
        }
        return digest;
    }

private:
    std::array<uint32_t, 8u> m_state {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    std::array<uint8_t, 64u> m_buffer {};
    size_t m_bufferedByteCount = 0u;
    uint64_t m_byteCount = 0u;
#if defined(_WIN32)
    BCRYPT_HASH_HANDLE m_windowsHash = nullptr;
#endif
};

std::array<uint8_t, 32u> Sha256(const uint8_t* bytes, const size_t byteCount)
{
    Sha256Hasher hasher;
    if (!hasher.Update(bytes, byteCount))
        return {};
    return hasher.Finalize();
}

std::string ToHex(const std::array<uint8_t, 32u>& bytes)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : bytes)
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    return stream.str();
}
}

const ImportedArtifact* ArtifactManifest::FindPrimaryArtifact() const
{
    return FindSubAsset(primarySubAssetKey);
}

const ImportedArtifact* ArtifactManifest::FindSubAsset(const std::string& subAssetKey) const
{
    for (const auto& artifact : subAssets)
    {
        if (artifact.subAssetKey == subAssetKey)
            return &artifact;
    }
    return nullptr;
}

bool IsContentStorageArtifactPath(const std::string& artifactPath)
{
    if (artifactPath.empty())
        return false;

    const auto path = std::filesystem::path(artifactPath);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;

    for (const auto& part : path)
    {
        if (part == "..")
            return false;
    }

    std::vector<std::string> parts;
    for (const auto& part : path)
        parts.push_back(part.generic_string());

    size_t firstPayloadPart = 0u;
    const bool hasEditorArtifactRoot =
        parts.size() >= 4u &&
        parts[0] == "Library" &&
        parts[1] == "Artifacts";
    if (hasEditorArtifactRoot)
        firstPayloadPart = 2u;

    const bool hasRuntimeArtifactRoot =
        parts.size() >= 3u &&
        parts[0] == "Artifacts";
    if (hasRuntimeArtifactRoot)
        firstPayloadPart = 1u;

    if (!hasEditorArtifactRoot && !hasRuntimeArtifactRoot)
        return false;

    if (parts.size() != firstPayloadPart + 2u)
        return false;

    const auto shard = parts[firstPayloadPart];
    const auto fileName = path.filename().generic_string();
    if (shard.size() != 2u ||
        !std::all_of(shard.begin(), shard.end(), [](const unsigned char character)
        {
            return std::isxdigit(character) != 0;
        }))
    {
        return false;
    }

    return IsArtifactStorageFileName(fileName) &&
        fileName.rfind(shard, 0u) == 0u;
}

std::string TryMakePortableContentArtifactPath(const std::string& artifactPath)
{
    if (artifactPath.empty())
        return {};

    const auto normalized = std::filesystem::path(artifactPath).lexically_normal();
    if (IsContentStorageArtifactPath(normalized.generic_string()))
        return normalized.generic_string();

    std::vector<std::string> parts;
    for (const auto& part : normalized)
        parts.push_back(part.generic_string());

    for (size_t index = 0u; index + 1u < parts.size(); ++index)
    {
        if (parts[index] == "Library" && parts[index + 1u] == "Artifacts")
        {
            std::filesystem::path portable;
            for (size_t partIndex = index; partIndex < parts.size(); ++partIndex)
                portable /= parts[partIndex];

            const auto portableString = portable.generic_string();
            return IsContentStorageArtifactPath(portableString) ? portableString : std::string {};
        }

        if (parts[index] == "Artifacts")
        {
            std::filesystem::path portable;
            for (size_t partIndex = index; partIndex < parts.size(); ++partIndex)
                portable /= parts[partIndex];

            const auto portableString = portable.generic_string();
            return IsContentStorageArtifactPath(portableString) ? portableString : std::string {};
        }
    }

    return {};
}

bool IsArtifactStorageFileName(const std::string& fileName)
{
    if (fileName.empty())
        return false;

    const auto path = std::filesystem::path(fileName);
    if (path.has_parent_path() || path.has_extension())
        return false;

    if (fileName.size() != 64u)
        return false;

    return std::all_of(fileName.begin(), fileName.end(), [](const unsigned char character)
    {
        return std::isxdigit(character) != 0;
    });
}

std::filesystem::path BuildArtifactStorageRelativePath(const std::string_view storageFileName)
{
    if (!IsArtifactStorageFileName(std::string(storageFileName)))
        return {};
    return std::filesystem::path(std::string(storageFileName.substr(0u, 2u))) /
        std::string(storageFileName);
}

std::string BuildArtifactStorageFileName(const std::string_view storageKey)
{
    return BuildArtifactStorageFileName(
        reinterpret_cast<const uint8_t*>(storageKey.data()),
        storageKey.size());
}

std::string BuildArtifactStorageFileName(const uint8_t* bytes, const size_t byteCount)
{
    if (bytes == nullptr && byteCount > 0u)
        return {};

    return ToHex(Sha256(bytes, byteCount));
}

std::string BuildArtifactStorageFileNameFromFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};

    Sha256Hasher hasher;
    std::array<uint8_t, 256u * 1024u> buffer {};
    while (stream)
    {
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto byteCount = stream.gcount();
        if (byteCount > 0 && !hasher.Update(buffer.data(), static_cast<size_t>(byteCount)))
            return {};
    }
    if (stream.bad())
        return {};
    return ToHex(hasher.Finalize());
}

void ClearRuntimeArtifactAuthorization()
{
    std::lock_guard lock(g_runtimeArtifactAuthorizationMutex);
    g_runtimeAuthorizedArtifactPaths.clear();
    g_runtimeArtifactAuthorizationEnabled = false;
}

void RegisterRuntimeAuthorizedArtifactPath(const std::string& artifactPath)
{
    const auto normalized = NormalizePortableArtifactPath(artifactPath);
    if (!IsContentStorageArtifactPath(normalized))
        return;

    std::lock_guard lock(g_runtimeArtifactAuthorizationMutex);
    g_runtimeAuthorizedArtifactPaths.insert(normalized);
}

void SetRuntimeArtifactAuthorizationEnabled(const bool enabled)
{
    std::lock_guard lock(g_runtimeArtifactAuthorizationMutex);
    g_runtimeArtifactAuthorizationEnabled = enabled;
}

bool IsRuntimeArtifactAuthorizationEnabled()
{
    std::lock_guard lock(g_runtimeArtifactAuthorizationMutex);
    return g_runtimeArtifactAuthorizationEnabled;
}

bool IsRuntimeArtifactPathAuthorized(const std::string& artifactPath)
{
    const auto normalized = NormalizePortableArtifactPath(artifactPath);
    std::lock_guard lock(g_runtimeArtifactAuthorizationMutex);
    if (!g_runtimeArtifactAuthorizationEnabled)
        return true;
    return g_runtimeAuthorizedArtifactPaths.find(normalized) != g_runtimeAuthorizedArtifactPaths.end();
}
}
