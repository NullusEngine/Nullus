#include "ArchiveWriter.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#if defined(NLS_BUILD_HAS_ZLIB)
#include <zlib.h>
#endif

namespace NLS::Build
{
namespace
{
constexpr uint32_t kLocalFileHeaderSignature = 0x04034b50u;
constexpr uint32_t kCentralDirectorySignature = 0x02014b50u;
constexpr uint32_t kEndOfCentralDirectorySignature = 0x06054b50u;

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

uint32_t ComputeCrc32(const uint8_t* bytes, const size_t byteCount)
{
#if defined(NLS_BUILD_HAS_ZLIB)
    return static_cast<uint32_t>(crc32(0u, bytes, static_cast<uInt>(byteCount)));
#else
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0u; index < byteCount; ++index)
    {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
#endif
}

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
    return static_cast<bool>(stream) || stream.eof();
}

bool DeflateBuffer(const std::vector<uint8_t>& input, std::vector<uint8_t>& output)
{
#if defined(NLS_BUILD_HAS_ZLIB)
    z_stream stream {};
    if (deflateInit2(&stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;

    output.resize(static_cast<size_t>(deflateBound(&stream, static_cast<uLong>(input.size()))));
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    const auto result = deflate(&stream, Z_FINISH);
    const auto produced = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END)
        return false;
    output.resize(static_cast<size_t>(produced));
    return true;
#else
    (void)input;
    (void)output;
    return false;
#endif
}
} // namespace

ZipArchiveWriter::ZipArchiveWriter(std::filesystem::path destination)
    : m_destination(std::move(destination))
{
}

bool ZipArchiveWriter::AddFile(
    const std::string& archivePath,
    const std::filesystem::path& sourcePath,
    std::string* error,
    const bool executable)
{
    if (archivePath.empty())
    {
        if (error != nullptr)
            *error = "zip: empty archive path";
        return false;
    }
    std::vector<uint8_t> raw;
    if (!ReadWholeFile(sourcePath, raw))
    {
        if (error != nullptr)
            *error = "zip: cannot read '" + sourcePath.string() + "'";
        return false;
    }

    Entry entry;
    entry.archivePath = archivePath;
    entry.executable = executable;
    entry.crc = ComputeCrc32(raw.data(), raw.size());
    entry.rawSize = raw.size();

    std::vector<uint8_t> compressed;
    if (!raw.empty() && DeflateBuffer(raw, compressed) && compressed.size() < raw.size())
    {
        entry.compressed = true;
        entry.storedSize = compressed.size();
        m_payloads.push_back(std::move(compressed));
    }
    else
    {
        entry.compressed = false;
        entry.storedSize = raw.size();
        m_payloads.push_back(std::move(raw));
    }
    m_entries.push_back(std::move(entry));
    return true;
}

bool ZipArchiveWriter::AddDirectory(
    const std::string& archivePrefix,
    const std::filesystem::path& sourceDirectory,
    std::string* error,
    const std::function<bool(const std::string&)>& executable)
{
    std::error_code directoryError;
    if (!std::filesystem::is_directory(sourceDirectory, directoryError))
    {
        if (error != nullptr)
            *error = "zip: directory missing '" + sourceDirectory.string() + "'";
        return false;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceDirectory, directoryError))
    {
        if (directoryError)
            break;
        if (entry.is_regular_file())
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    auto prefix = archivePrefix;
    while (!prefix.empty() && prefix.back() == '/')
        prefix.pop_back();
    for (const auto& file : files)
    {
        std::error_code relativeError;
        const auto relative = std::filesystem::relative(file, sourceDirectory, relativeError);
        if (relativeError)
            continue;
        auto archivePath = relative.generic_string();
        if (!prefix.empty())
            archivePath = prefix + "/" + archivePath;
        if (!AddFile(
                archivePath,
                file,
                error,
                executable != nullptr && executable(archivePath)))
        {
            return false;
        }
    }
    return true;
}

bool ZipArchiveWriter::Write(std::string* error)
{
    if (m_entries.size() > 0xFFFFu)
    {
        if (error != nullptr)
            *error = "zip: more than 65535 entries requires ZIP64, which is not supported";
        return false;
    }
    std::error_code directoryError;
    if (!m_destination.parent_path().empty())
        std::filesystem::create_directories(m_destination.parent_path(), directoryError);

    const auto temporary = m_destination.string() + ".tmp";
    std::filesystem::remove(temporary, directoryError);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
            *error = "zip: cannot create '" + temporary + "'";
        return false;
    }

    // Fixed DOS timestamp (1980-01-01) keeps archived bytes deterministic.
    constexpr uint16_t kDosTime = 0u;
    constexpr uint16_t kDosDate = 0x0021u;

    const auto exceedsZip32 = [&output]()
    {
        const auto position = output.tellp();
        return position < 0 || static_cast<uint64_t>(position) > 0xFFFFFFFFull;
    };

    std::vector<uint8_t> header;
    for (size_t index = 0u; index < m_entries.size(); ++index)
    {
        auto& entry = m_entries[index];
        entry.localHeaderOffset = static_cast<uint64_t>(output.tellp());
        header.clear();
        AppendU32(header, kLocalFileHeaderSignature);
        AppendU16(header, 20u);                         // version needed
        AppendU16(header, 0u);                          // flags
        AppendU16(header, entry.compressed ? 8u : 0u);  // method
        AppendU16(header, kDosTime);
        AppendU16(header, kDosDate);
        AppendU32(header, entry.crc);
        AppendU32(header, static_cast<uint32_t>(entry.storedSize));
        AppendU32(header, static_cast<uint32_t>(entry.rawSize));
        AppendU16(header, static_cast<uint16_t>(entry.archivePath.size()));
        AppendU16(header, 0u);                          // extra length
        output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        output.write(entry.archivePath.data(), static_cast<std::streamsize>(entry.archivePath.size()));
        const auto& payload = m_payloads[index];
        if (!payload.empty())
            output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!output)
        {
            if (error != nullptr)
                *error = "zip: write failed";
            return false;
        }
    }

    if (exceedsZip32())
    {
        if (error != nullptr)
            *error = "zip: archive exceeds 4 GiB; ZIP64 output is not supported";
        return false;
    }

    const auto centralDirectoryOffset = static_cast<uint64_t>(output.tellp());
    for (const auto& entry : m_entries)
    {
        header.clear();
        AppendU32(header, kCentralDirectorySignature);
        // Unix (3) host, ZIP 2.0. The high 16 bits of the external attributes
        // carry st_mode so unzip restores the executable bit on Linux/macOS.
        AppendU16(header, 0x0314u);                     // version made by
        AppendU16(header, 20u);                         // version needed
        AppendU16(header, 0u);                          // flags
        AppendU16(header, entry.compressed ? 8u : 0u);  // method
        AppendU16(header, kDosTime);
        AppendU16(header, kDosDate);
        AppendU32(header, entry.crc);
        AppendU32(header, static_cast<uint32_t>(entry.storedSize));
        AppendU32(header, static_cast<uint32_t>(entry.rawSize));
        AppendU16(header, static_cast<uint16_t>(entry.archivePath.size()));
        AppendU16(header, 0u);                          // extra length
        AppendU16(header, 0u);                          // comment length
        AppendU16(header, 0u);                          // disk number
        AppendU16(header, 0u);                          // internal attributes
        AppendU32(header, (entry.executable ? 0100755u : 0100644u) << 16u);
        AppendU32(header, static_cast<uint32_t>(entry.localHeaderOffset));
        output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        output.write(entry.archivePath.data(), static_cast<std::streamsize>(entry.archivePath.size()));
    }

    const auto centralDirectorySize = static_cast<uint64_t>(output.tellp()) - centralDirectoryOffset;
    if (centralDirectoryOffset > 0xFFFFFFFFull || centralDirectorySize > 0xFFFFFFFFull)
    {
        if (error != nullptr)
            *error = "zip: archive exceeds 4 GiB; ZIP64 output is not supported";
        return false;
    }
    header.clear();
    AppendU32(header, kEndOfCentralDirectorySignature);
    AppendU16(header, 0u);
    AppendU16(header, 0u);
    AppendU16(header, static_cast<uint16_t>(m_entries.size()));
    AppendU16(header, static_cast<uint16_t>(m_entries.size()));
    AppendU32(header, static_cast<uint32_t>(centralDirectorySize));
    AppendU32(header, static_cast<uint32_t>(centralDirectoryOffset));
    AppendU16(header, 0u);
    output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    output.flush();
    if (!output)
    {
        if (error != nullptr)
            *error = "zip: finalise failed";
        return false;
    }
    output.close();

    std::error_code renameError;
    std::filesystem::remove(m_destination, renameError);
    std::filesystem::rename(temporary, m_destination, renameError);
    if (renameError)
    {
        std::filesystem::remove(temporary, renameError);
        if (error != nullptr)
            *error = "zip: cannot commit '" + m_destination.string() + "'";
        return false;
    }
    return true;
}
} // namespace NLS::Build
