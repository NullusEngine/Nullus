#pragma once

#include "CoreDef.h"
#include "Assets/AssetId.h"

#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Core::Assets
{
enum class AssetDiagnosticSeverity
{
    Info,
    Warning,
    Error
};

struct NLS_CORE_API AssetDiagnostic
{
    AssetDiagnosticSeverity severity = AssetDiagnosticSeverity::Info;
    std::string code;
    AssetId assetId;
    std::filesystem::path path;
    std::string message;
};

using AssetDiagnostics = std::vector<AssetDiagnostic>;
}
