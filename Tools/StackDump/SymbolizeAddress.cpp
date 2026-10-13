// Resolves module-relative addresses against a PDB.
//
// A structured exception reports the faulting module and offset. A raw offset is
// not actionable on its own, and a live-process stack walk cannot reach a fault
// that was already caught, so this turns the logged offsets into function names,
// source files and line numbers.
//
// Usage:
//   SymbolizeAddress.exe <binary> <hexOffset> [<hexOffset> ...] [symbolPath=<dir>]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace
{
    constexpr const char* kSymbolPathPrefix = "symbolPath=";

    bool TryParseSymbolPath(const char* const argument, std::string& outPath)
    {
        const size_t prefixLength = std::strlen(kSymbolPathPrefix);
        if (std::strncmp(argument, kSymbolPathPrefix, prefixLength) != 0)
            return false;
        outPath = argument + prefixLength;
        return true;
    }

    /**
     * Reads the preferred base and image size straight from the file.
     *
     * DbgHelp needs both to translate an offset: the base to place the module in
     * its address space, and the size to recognise which addresses belong to it.
     * A `LoadLibraryEx(LOAD_LIBRARY_AS_DATAFILE)` handle is not a load address, so
     * the headers have to come from the bytes on disk.
     */
    bool ReadImageLayout(
        const char* const binaryPath,
        DWORD64& outImageBase,
        DWORD32& outImageSize)
    {
        HANDLE file = CreateFileA(
            binaryPath,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0u, 0u, nullptr);
        if (mapping == nullptr)
        {
            CloseHandle(file);
            return false;
        }

        bool succeeded = false;
        const auto* const view = static_cast<const unsigned char*>(
            MapViewOfFile(mapping, FILE_MAP_READ, 0u, 0u, 0u));
        if (view != nullptr)
        {
            const auto* const dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(view);
            if (dosHeader->e_magic == IMAGE_DOS_SIGNATURE)
            {
                const auto* const ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                    view + dosHeader->e_lfanew);
                if (ntHeaders->Signature == IMAGE_NT_SIGNATURE)
                {
                    outImageBase = ntHeaders->OptionalHeader.ImageBase;
                    outImageSize = ntHeaders->OptionalHeader.SizeOfImage;
                    succeeded = true;
                }
            }
            UnmapViewOfFile(view);
        }

        CloseHandle(mapping);
        CloseHandle(file);
        return succeeded;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: SymbolizeAddress.exe <binary> <hexOffset> [<hexOffset> ...] [symbolPath=<dir>]\n");
        return 2;
    }

    const char* const binaryPath = argv[1];

    std::string symbolPath;
    for (int index = 3; index < argc; ++index)
        (void)TryParseSymbolPath(argv[index], symbolPath);

    SymSetOptions(
        SYMOPT_CASE_INSENSITIVE | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
        SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);

    DWORD64 imageBase = 0u;
    DWORD32 imageSize = 0u;
    if (!ReadImageLayout(binaryPath, imageBase, imageSize))
    {
        std::printf("Failed to read the PE headers of '%s'\n", binaryPath);
        return 1;
    }

    const HANDLE process = GetCurrentProcess();
    const char* const searchPath = symbolPath.empty() ? binaryPath : symbolPath.c_str();
    if (!SymInitialize(process, searchPath, FALSE))
    {
        std::printf("SymInitialize(%s) failed, gle=%lu\n", searchPath, GetLastError());
        return 1;
    }

    // The preferred base is the right choice for an image that is not actually
    // mapped: DbgHelp places the symbols there and every offset resolves against it.
    const DWORD64 moduleBase = imageBase;
    if (SymLoadModuleEx(
            process,
            nullptr,
            binaryPath,
            nullptr,
            moduleBase,
            imageSize,
            nullptr,
            0u) == 0u)
    {
        std::printf("SymLoadModuleEx failed, gle=%lu\n", GetLastError());
        SymCleanup(process);
        return 1;
    }

    std::printf(
        "module=%s\nimageBase=0x%llX\n",
        binaryPath,
        static_cast<unsigned long long>(moduleBase));

    std::vector<char> storage(sizeof(SYMBOL_INFO) + 2048u, 0);
    auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 2048u;

    for (int index = 2; index < argc; ++index)
    {
        std::string argument = argv[index];
        if (TryParseSymbolPath(argv[index], argument))
            continue;

        const unsigned long long offset = std::strtoull(argument.c_str(), nullptr, 16);
        const DWORD64 address = moduleBase + offset;
        std::printf("\n+0x%llX\n", offset);

        DWORD64 displacement = 0u;
        if (SymFromAddr(process, address, &displacement, symbol))
        {
            std::printf(
                "  symbol=%s+0x%llX\n",
                symbol->Name,
                static_cast<unsigned long long>(displacement));
        }
        else
        {
            std::printf("  symbol=<unresolved, gle=%lu>\n", GetLastError());
        }

        IMAGEHLP_LINE64 line {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0u;
        if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line))
        {
            std::printf("  file=%s @ %lu\n", line.FileName, line.LineNumber);
        }
        else
        {
            std::printf("  file=<no line info, gle=%lu>\n", GetLastError());
        }
    }

    SymCleanup(process);
    return 0;
}
