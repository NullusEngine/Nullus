#pragma once

#include "BuildDef.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace NLS::Build
{
// Minimal deterministic ZIP writer used by the Windows desktop output.
//
// Deflate is used when the build links zlib; otherwise entries are stored.
// Timestamps are fixed and entries are written in the order they are added, so
// the same input produces the same archive bytes.
class NLS_BUILD_API ZipArchiveWriter
{
public:
    explicit ZipArchiveWriter(std::filesystem::path destination);

    // archivePath uses forward slashes and is stored verbatim.
    // executable marks the entry with Unix mode 0755 so extraction preserves
    // the bit on Linux/macOS.
    bool AddFile(
        const std::string& archivePath,
        const std::filesystem::path& sourcePath,
        std::string* error,
        bool executable = false);
    bool AddDirectory(
        const std::string& archivePrefix,
        const std::filesystem::path& sourceDirectory,
        std::string* error,
        // Receives the final archive path (including archivePrefix).
        const std::function<bool(const std::string&)>& executable = {});
    bool Write(std::string* error);

    size_t GetEntryCount() const { return m_entries.size(); }

private:
    struct Entry
    {
        std::string archivePath;
        uint32_t crc = 0u;
        uint64_t storedSize = 0u;
        uint64_t rawSize = 0u;
        uint64_t localHeaderOffset = 0u;
        bool compressed = false;
        bool executable = false;
    };

    std::filesystem::path m_destination;
    std::vector<Entry> m_entries;
    std::vector<std::vector<uint8_t>> m_payloads;
};
} // namespace NLS::Build
