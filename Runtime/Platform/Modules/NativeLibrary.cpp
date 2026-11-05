#include "NativeLibrary.h"
#include <stdexcept>
#include <fstream>
#include <array>
#include <algorithm>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace NLS::Platform
{
// PE32+ offsets and RVA semantics follow the Microsoft PE/COFF specification:
// https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
std::vector<std::string> NativeLibrary::WindowsImports(const std::filesystem::path& path)
{
    std::ifstream input(path,std::ios::binary); const auto size = std::filesystem::file_size(path);
    const auto read = [&](uint64_t offset,size_t count) {
        if (offset > size || count > size-offset || count > 4096) throw std::runtime_error("invalid PE range");
        std::vector<uint8_t> bytes(count); input.clear(); input.seekg(offset);
        if (!input.read(reinterpret_cast<char*>(bytes.data()),count)) throw std::runtime_error("unreadable PE range"); return bytes;
    };
    const auto u16 = [](const std::vector<uint8_t>& b,size_t p) { return uint16_t(b.at(p)) | uint16_t(b.at(p+1)) << 8; };
    const auto u32 = [](const std::vector<uint8_t>& b,size_t p) { return uint32_t(b.at(p)) | uint32_t(b.at(p+1)) << 8 | uint32_t(b.at(p+2)) << 16 | uint32_t(b.at(p+3)) << 24; };
    const auto dos=read(0,64); if (dos[0]!='M' || dos[1]!='Z') throw std::runtime_error("not a PE binary");
    const auto peOffset=u32(dos,60); const auto pe=read(peOffset,24);
    if (u32(pe,0)!=0x4550) throw std::runtime_error("invalid PE signature");
    const auto sectionCount=u16(pe,6), optionalSize=u16(pe,20);
    if (sectionCount > 96) throw std::runtime_error("PE section limit exceeded");
    const auto optional=read(uint64_t(peOffset)+24,optionalSize);
    if (u16(optional,0)!=0x20b || optional.size()<112) throw std::runtime_error("only PE32+ dependency inspection is supported");
    const auto sectionBytes=read(uint64_t(peOffset)+24+optionalSize,sectionCount*40);
    const auto offsetFor = [&](uint64_t rva,size_t count) -> uint64_t {
        if (rva < u32(optional,60) && count <= u32(optional,60)-rva) return rva;
        for (uint32_t section=0; section<sectionCount; ++section)
        {
            const uint64_t base=u32(sectionBytes,section*40+12), rawSize=u32(sectionBytes,section*40+16);
            if (rva >= base && rva-base <= rawSize && count <= rawSize-(rva-base)) return u32(sectionBytes,section*40+20)+(rva-base);
        }
        throw std::runtime_error("PE import RVA has no file backing");
    };
    std::vector<std::string> imports;
    for (const auto directory : {1u,13u})
    {
        if (u32(optional,108)<=directory) continue;
        const auto rva=u32(optional,112+directory*8); if (!rva) continue;
        const size_t stride=directory==1 ? 20 : 32; bool terminated=false;
        for (uint32_t index=0; index<4096; ++index)
        {
            const auto descriptor=read(offsetFor(uint64_t(rva)+index*stride,stride),stride);
            if (std::all_of(descriptor.begin(),descriptor.end(),[](auto b) { return b==0; })) { terminated=true; break; }
            if (directory==13 && u32(descriptor,0)!=1) throw std::runtime_error("legacy absolute delay import descriptor is unsupported");
            const auto nameOffset=offsetFor(u32(descriptor,directory==1 ? 12 : 4),1);
            const auto bytes=read(nameOffset,static_cast<size_t>((std::min)(uint64_t{256},size-nameOffset)));
            std::string name; bool ended=false;
            for (auto c : bytes)
            {
                if (!c) { ended=true; break; }
                if (!((c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9') || c=='_' || c=='-' || c=='.'))
                    throw std::runtime_error("PE import must be a plain ASCII library name");
                name += c>='A'&&c<='Z' ? c+'a'-'A' : c;
            }
            if (!ended || name.empty()) throw std::runtime_error("invalid PE import name"); imports.push_back(std::move(name));
        }
        if (!terminated) throw std::runtime_error("PE import descriptor limit exceeded");
    }
    std::sort(imports.begin(),imports.end()); imports.erase(std::unique(imports.begin(),imports.end()),imports.end()); return imports;
}
bool NativeLibrary::IsWindowsSystemLibrary(const std::string& name)
{
    if (name.rfind("api-ms-win-",0)==0 || name.rfind("ext-ms-win-",0)==0) return true;
#ifdef _WIN32
    wchar_t system[32768]{}; if (!GetSystemDirectoryW(system,32768)) return false;
    return std::filesystem::is_regular_file(std::filesystem::path(system) / name);
#else
    return false;
#endif
}
void NativeLibrary::ValidateTarget(const std::filesystem::path& path,const std::string& platform,const std::string& architecture)
{
    std::ifstream input(path,std::ios::binary); std::array<uint8_t,64> header{};
    if (!input.read(reinterpret_cast<char*>(header.data()),header.size())) throw std::runtime_error("plugin binary header is incomplete");
    const auto u16 = [](const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; };
    const auto u32 = [](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
    if (architecture != "x64" && architecture != "arm64") throw std::runtime_error("unsupported plugin architecture");
    bool compatible = false;
    if (platform == "Windows" && header[0] == 'M' && header[1] == 'Z')
    {
        const auto offset = u32(header.data()+60);
        if (offset > 16*1024*1024) throw std::runtime_error("invalid PE header offset");
        input.seekg(offset); std::array<uint8_t,26> pe{};
        if (input.read(reinterpret_cast<char*>(pe.data()),pe.size()))
            compatible = u32(pe.data()) == 0x00004550 && u16(pe.data()+4) == (architecture == "arm64" ? 0xaa64 : 0x8664) &&
                (u16(pe.data()+22) & 0x2000) && u16(pe.data()+24) == 0x020b;
    }
    else if (platform == "Linux")
        compatible = header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' && header[3] == 'F' && header[4] == 2 && header[5] == 1 &&
            u16(header.data()+16) == 3 && u16(header.data()+18) == (architecture == "arm64" ? 183 : 62);
    else if (platform == "macOS")
        compatible = u32(header.data()) == 0xfeedfacf && u32(header.data()+4) == (architecture == "arm64" ? 0x0100000c : 0x01000007) &&
            (u32(header.data()+12) == 6 || u32(header.data()+12) == 8);
    if (!compatible) throw std::runtime_error("plugin binary OS/architecture/format mismatch: " + path.generic_string());
}
NativeLibrary::NativeLibrary(const std::filesystem::path& path)
{
    if (!path.is_absolute() || !std::filesystem::is_regular_file(path))
        throw std::runtime_error("plugin library must be an existing absolute file: " + path.generic_string());
    m_path = std::filesystem::canonical(path);
#if defined(_M_ARM64) || defined(__aarch64__)
    const std::string architecture = "arm64";
#else
    const std::string architecture = "x64";
#endif
#ifdef _WIN32
    ValidateTarget(m_path,"Windows",architecture);
    for (const auto& dependency : WindowsImports(m_path))
        if (dependency.rfind("nls_",0)==0) throw std::runtime_error("Binary Plugin imports engine implementation: " + dependency);
#elif defined(__APPLE__)
    ValidateTarget(m_path,"macOS",architecture);
#else
    ValidateTarget(m_path,"Linux",architecture);
#endif
#ifdef _WIN32
    m_handle = LoadLibraryExW(m_path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m_handle) throw std::runtime_error("LoadLibraryEx failed (" + std::to_string(GetLastError()) + "): " + m_path.generic_string());
#else
    m_handle = dlopen(m_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m_handle) throw std::runtime_error("dlopen failed: " + std::string(dlerror()));
#endif
}
NativeLibrary::~NativeLibrary()
{
    if (!m_handle || !m_canUnload) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(m_handle));
#else
    dlclose(m_handle);
#endif
}
void* NativeLibrary::Symbol(const char* name) const
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
    return dlsym(m_handle, name);
#endif
}
}
