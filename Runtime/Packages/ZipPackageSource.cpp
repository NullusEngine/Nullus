#include "PackageManager.h"
#include "Core/Assets/PluginAssetPath.h"
#include "unzip.h"
#include "ioapi.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>

namespace NLS::Packages
{
namespace
{
voidpf ZCALLBACK OpenArchive(voidpf, const void* filename, int)
{
    const auto& path = *static_cast<const std::filesystem::path*>(filename);
#ifdef _WIN32
    return _wfopen(path.c_str(),L"rb");
#else
    return std::fopen(path.c_str(),"rb");
#endif
}
}
void ZipPackageSource::Fetch(const PackageRequest& request, const std::filesystem::path& destination, const PackageLimits& limits)
{
    const auto archivePath = std::filesystem::u8path(request.location);
    if (std::filesystem::file_size(archivePath) > limits.maxDownloadBytes) throw std::runtime_error("ZIP download size limit exceeded");
    zlib_filefunc64_def functions{}; fill_fopen64_filefunc(&functions); functions.zopen64_file = OpenArchive;
    const auto archive = unzOpen2_64(&archivePath,&functions);
    if (!archive) throw std::runtime_error("cannot open package ZIP");
    struct ArchiveGuard { unzFile file; ~ArchiveGuard() { unzClose(file); } } guard{archive};
    uint64_t expanded = 0; uint32_t files = 0; std::set<std::string> aliases;
    std::filesystem::create_directories(destination);
    int status = unzGoToFirstFile(archive);
    while (status == UNZ_OK)
    {
        if (request.cancel && request.cancel->load()) throw std::runtime_error("package installation cancelled");
        if (++files > limits.maxFiles) throw std::runtime_error("ZIP file count limit exceeded");
        unz_file_info64 info{};
        if (unzGetCurrentFileInfo64(archive,&info,nullptr,0,nullptr,0,nullptr,0) != UNZ_OK || !info.size_filename || info.size_filename > 65535)
            throw std::runtime_error("invalid ZIP entry metadata");
        std::string raw(info.size_filename + 1,0);
        if (unzGetCurrentFileInfo64(archive,&info,raw.data(),static_cast<uLong>(raw.size()),nullptr,0,nullptr,0) != UNZ_OK) throw std::runtime_error("cannot read ZIP entry name");
        raw.resize(info.size_filename);
        if (raw.find('\0') != std::string::npos || (info.flag & 1)) throw std::runtime_error("encrypted or malformed ZIP entry");
        if (!(info.flag & 0x800) && std::any_of(raw.begin(),raw.end(),[](unsigned char c) { return c >= 128; })) throw std::runtime_error("non-UTF8 ZIP names are unsupported");
        const auto unixType = (info.external_fa >> 16) & 0170000;
        if (unixType && unixType != 0100000 && unixType != 0040000) throw std::runtime_error("ZIP links and device entries are forbidden");
        const bool directory = raw.back() == '/' || raw.back() == '\\' || unixType == 0040000;
        const auto normalized = Core::Assets::NormalizePluginAssetPath(raw);
        for (const auto& part : std::filesystem::u8path(normalized))
            if (Core::Assets::PluginAssetCollisionKey(part.generic_string()) == ".git") throw std::runtime_error("ZIP Git metadata is forbidden");
        if (!aliases.insert(Core::Assets::PluginAssetCollisionKey(normalized)).second) throw std::runtime_error("duplicate/case-conflicting ZIP entry");
        const auto output = destination / std::filesystem::u8path(normalized);
        if (directory) std::filesystem::create_directories(output);
        else
        {
            if (info.uncompressed_size > limits.maxExpandedBytes - expanded) throw std::runtime_error("ZIP expanded size limit exceeded");
            if (unzOpenCurrentFile(archive) != UNZ_OK) throw std::runtime_error("cannot open ZIP entry");
            struct EntryGuard { unzFile file; bool open = true; ~EntryGuard() { if (open) unzCloseCurrentFile(file); } } entry{archive};
            std::filesystem::create_directories(output.parent_path()); std::ofstream stream(output,std::ios::binary|std::ios::trunc);
            if (!stream) throw std::runtime_error("cannot extract package file");
            uint64_t size = 0; std::array<char,65536> buffer{};
            for (;;)
            {
                if (request.cancel && request.cancel->load()) throw std::runtime_error("package installation cancelled");
                const auto count = unzReadCurrentFile(archive,buffer.data(),static_cast<unsigned>(buffer.size()));
                if (count < 0) throw std::runtime_error("corrupt ZIP entry"); if (!count) break;
                if (static_cast<uint64_t>(count) > limits.maxExpandedBytes - expanded) throw std::runtime_error("ZIP expanded size limit exceeded");
                expanded += count; size += count; stream.write(buffer.data(),count);
                if (!stream) throw std::runtime_error("package extraction write failed");
            }
            if (size != info.uncompressed_size) throw std::runtime_error("ZIP entry size mismatch");
            const auto closed = unzCloseCurrentFile(archive); entry.open = false;
            if (closed != UNZ_OK) throw std::runtime_error("ZIP entry CRC mismatch");
        }
        status = unzGoToNextFile(archive);
    }
    if (status != UNZ_END_OF_LIST_OF_FILE) throw std::runtime_error("corrupt ZIP directory");
}
}
