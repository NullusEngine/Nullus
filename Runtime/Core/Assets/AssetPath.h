#pragma once

#include "CoreDef.h"

#include <filesystem>

namespace NLS::Core::Assets
{
NLS_CORE_API std::filesystem::path NormalizeAssetPath(const std::filesystem::path& path);
NLS_CORE_API bool IsMetaFilePath(const std::filesystem::path& path);
}

