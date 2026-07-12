#include "Assets/ArtifactManifest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

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

std::array<uint8_t, 32u> Sha256(const uint8_t* bytes, const size_t byteCount)
{
    static constexpr std::array<uint32_t, 64u> kConstants {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    const uint64_t bitCount = static_cast<uint64_t>(byteCount) * 8ull;
    const size_t remainderAfterOne = (byteCount + 1u) % 64u;
    const size_t zeroPaddingBytes = remainderAfterOne <= 56u
        ? 56u - remainderAfterOne
        : 64u + 56u - remainderAfterOne;
    const size_t paddedSize = byteCount + 1u + zeroPaddingBytes + 8u;

    uint32_t h0 = 0x6a09e667u;
    uint32_t h1 = 0xbb67ae85u;
    uint32_t h2 = 0x3c6ef372u;
    uint32_t h3 = 0xa54ff53au;
    uint32_t h4 = 0x510e527fu;
    uint32_t h5 = 0x9b05688cu;
    uint32_t h6 = 0x1f83d9abu;
    uint32_t h7 = 0x5be0cd19u;

    for (size_t offset = 0u; offset < paddedSize; offset += 64u)
    {
        std::array<uint8_t, 64u> block {};
        for (size_t index = 0u; index < block.size(); ++index)
        {
            const size_t byteIndex = offset + index;
            if (byteIndex < byteCount)
            {
                block[index] = bytes[byteIndex];
            }
            else if (byteIndex == byteCount)
            {
                block[index] = 0x80u;
            }
            else if (byteIndex >= paddedSize - 8u)
            {
                const size_t lengthByteIndex = byteIndex - (paddedSize - 8u);
                const int shift = static_cast<int>((7u - lengthByteIndex) * 8u);
                block[index] = static_cast<uint8_t>((bitCount >> shift) & 0xffu);
            }
        }

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
            const uint32_t s0 = RotateRight(words[index - 15u], 7u) ^ RotateRight(words[index - 15u], 18u) ^ (words[index - 15u] >> 3u);
            const uint32_t s1 = RotateRight(words[index - 2u], 17u) ^ RotateRight(words[index - 2u], 19u) ^ (words[index - 2u] >> 10u);
            words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        uint32_t f = h5;
        uint32_t g = h6;
        uint32_t h = h7;

        for (size_t index = 0u; index < 64u; ++index)
        {
            const uint32_t s1 = RotateRight(e, 6u) ^ RotateRight(e, 11u) ^ RotateRight(e, 25u);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + ch + kConstants[index] + words[index];
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

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }

    const std::array<uint32_t, 8u> state { h0, h1, h2, h3, h4, h5, h6, h7 };
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
