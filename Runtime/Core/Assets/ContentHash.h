#pragma once
#include "CoreDef.h"
#include <filesystem>
#include <string>

namespace NLS::Core::Assets
{
NLS_CORE_API std::string HashContentFile(const std::filesystem::path& path);
NLS_CORE_API std::string HashContentDirectory(const std::filesystem::path& root,
    uint32_t maxFiles = 100000, uint64_t maxBytes = UINT64_C(4) * 1024 * 1024 * 1024, bool ignoreVcsMetadata = false);
}
