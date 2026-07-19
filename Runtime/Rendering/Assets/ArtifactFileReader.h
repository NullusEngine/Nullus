#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace NLS::Render::Assets::Detail
{
struct ArtifactFileReadRange
{
    void* destination = nullptr;
    size_t byteSize = 0u;
};

struct ArtifactFileMapping
{
    std::shared_ptr<void> owner;
    const uint8_t* data = nullptr;
    size_t size = 0u;
};

#if defined(_WIN32)
struct ScopedFileHandle
{
    HANDLE value = INVALID_HANDLE_VALUE;

    ScopedFileHandle() = default;
    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;

    ~ScopedFileHandle()
    {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
#endif

class ArtifactFileReader
{
public:
    explicit ArtifactFileReader(const std::filesystem::path& path)
    {
#if defined(_WIN32)
        m_file.value = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (m_file.value == INVALID_HANDLE_VALUE)
            return;

        LARGE_INTEGER fileSize {};
        if (!GetFileSizeEx(m_file.value, &fileSize) || fileSize.QuadPart < 0)
            return;
        m_size = static_cast<uint64_t>(fileSize.QuadPart);
#else
        m_input.open(path, std::ios::binary | std::ios::ate);
        if (!m_input)
            return;

        const auto endPosition = m_input.tellg();
        const auto fileSize = static_cast<std::streamoff>(endPosition);
        if (fileSize < 0)
            return;
        m_size = static_cast<uint64_t>(fileSize);
#endif
        m_valid = true;
    }

    bool IsOpen() const { return m_valid; }
    uint64_t Size() const { return m_size; }

    bool Read(
        const uint64_t fileOffset,
        const std::span<const ArtifactFileReadRange> ranges,
        const std::atomic_bool* cancellationFlag)
    {
        const auto isCancelled = [cancellationFlag]
        {
            return cancellationFlag != nullptr &&
                cancellationFlag->load(std::memory_order_acquire);
        };
        if (!m_valid || isCancelled())
            return false;

        uint64_t totalByteSize = 0u;
        for (const auto& range : ranges)
        {
            if ((range.byteSize > 0u && range.destination == nullptr) ||
                range.byteSize > (std::numeric_limits<uint64_t>::max)() - totalByteSize)
            {
                return false;
            }
            totalByteSize += range.byteSize;
        }
        if (fileOffset > m_size || totalByteSize > m_size - fileOffset)
            return false;

#if defined(_WIN32)
        LARGE_INTEGER seekOffset {};
        seekOffset.QuadPart = static_cast<LONGLONG>(fileOffset);
        if (!SetFilePointerEx(m_file.value, seekOffset, nullptr, FILE_BEGIN))
            return false;
#else
        if (fileOffset > static_cast<uint64_t>((std::numeric_limits<std::streamoff>::max)()))
            return false;
        m_input.clear();
        m_input.seekg(static_cast<std::streamoff>(fileOffset), std::ios::beg);
        if (!m_input)
            return false;
#endif

        constexpr size_t kReadChunkBytes = 8u * 1024u * 1024u;
        for (const auto& range : ranges)
        {
            size_t rangeOffset = 0u;
            while (rangeOffset < range.byteSize)
            {
                if (isCancelled())
                    return false;

                const auto chunkSize = std::min(
                    kReadChunkBytes,
                    range.byteSize - rangeOffset);
                auto* destination = static_cast<uint8_t*>(range.destination) + rangeOffset;
#if defined(_WIN32)
                DWORD bytesRead = 0u;
                if (!ReadFile(
                        m_file.value,
                        destination,
                        static_cast<DWORD>(chunkSize),
                        &bytesRead,
                        nullptr) ||
                    bytesRead != chunkSize)
                {
                    return false;
                }
#else
                m_input.read(
                    reinterpret_cast<char*>(destination),
                    static_cast<std::streamsize>(chunkSize));
                if (m_input.gcount() != static_cast<std::streamsize>(chunkSize))
                    return false;
#endif
                rangeOffset += chunkSize;
            }
        }
        return !isCancelled();
    }

private:
#if defined(_WIN32)
    ScopedFileHandle m_file;
#else
    std::ifstream m_input;
#endif
    uint64_t m_size = 0u;
    bool m_valid = false;
};

inline bool ReadArtifactFileBytes(
    const std::filesystem::path& path,
    std::vector<uint8_t>& bytes,
    const std::atomic_bool* cancellationFlag)
{
    bytes.clear();
    ArtifactFileReader reader(path);
    if (!reader.IsOpen() ||
        reader.Size() == 0u ||
        reader.Size() > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
    {
        return false;
    }

    bytes.resize(static_cast<size_t>(reader.Size()));
    const ArtifactFileReadRange range {bytes.data(), bytes.size()};
    if (!reader.Read(0u, std::span<const ArtifactFileReadRange>(&range, 1u), cancellationFlag))
    {
        bytes.clear();
        return false;
    }
    return true;
}

inline std::optional<ArtifactFileMapping> MapArtifactFileReadOnly(
    const std::filesystem::path& path,
    const std::atomic_bool* cancellationFlag)
{
#if defined(_WIN32)
    const auto isCancelled = [cancellationFlag]
    {
        return cancellationFlag != nullptr &&
            cancellationFlag->load(std::memory_order_acquire);
    };
    if (isCancelled())
        return std::nullopt;

    struct MappingState
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        HANDLE mapping = nullptr;
        void* view = nullptr;

        ~MappingState()
        {
            if (view != nullptr)
                UnmapViewOfFile(view);
            if (mapping != nullptr)
                CloseHandle(mapping);
            if (file != INVALID_HANDLE_VALUE)
                CloseHandle(file);
        }
    };

    auto state = std::make_shared<MappingState>();
    state->file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (state->file == INVALID_HANDLE_VALUE)
        return std::nullopt;

    LARGE_INTEGER fileSize {};
    if (!GetFileSizeEx(state->file, &fileSize) ||
        fileSize.QuadPart <= 0 ||
        static_cast<uint64_t>(fileSize.QuadPart) >
            static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
    {
        return std::nullopt;
    }

    state->mapping = CreateFileMappingW(
        state->file,
        nullptr,
        PAGE_READONLY,
        0,
        0,
        nullptr);
    if (state->mapping == nullptr)
        return std::nullopt;
    state->view = MapViewOfFile(state->mapping, FILE_MAP_READ, 0, 0, 0);
    if (state->view == nullptr || isCancelled())
        return std::nullopt;

    return ArtifactFileMapping {
        state,
        static_cast<const uint8_t*>(state->view),
        static_cast<size_t>(fileSize.QuadPart)
    };
#else
    (void)path;
    (void)cancellationFlag;
    return std::nullopt;
#endif
}

inline bool ReadArtifactFileRanges(
    const std::filesystem::path& path,
    const uint64_t fileOffset,
    const std::span<const ArtifactFileReadRange> ranges,
    const std::atomic_bool* cancellationFlag)
{
    ArtifactFileReader reader(path);
    return reader.Read(fileOffset, ranges, cancellationFlag);
}
}
