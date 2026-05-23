#pragma once

#include "Assets/AssetId.h"
#include "CoreDef.h"

#include <string>
#include <vector>

namespace NLS::Core::Assets
{
enum class ImportDiagnosticSeverity
{
    Info,
    Warning,
    Error
};

struct ImportDiagnostic
{
    ImportDiagnosticSeverity severity = ImportDiagnosticSeverity::Info;
    std::string code;
    AssetId assetId;
    std::string subAssetKey;
    std::string path;
    bool sticky = false;

    bool IsError() const
    {
        return severity == ImportDiagnosticSeverity::Error;
    }
};

class NLS_CORE_API ImportDiagnosticList
{
public:
    void Add(ImportDiagnostic diagnostic);
    bool HasErrors() const;
    const std::vector<ImportDiagnostic>& GetItems() const;
    bool Empty() const;

private:
    std::vector<ImportDiagnostic> m_items;
};
}
