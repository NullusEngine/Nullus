#include "Assets/ImportDiagnostics.h"

namespace NLS::Core::Assets
{
void ImportDiagnosticList::Add(ImportDiagnostic diagnostic)
{
    m_items.push_back(std::move(diagnostic));
}

bool ImportDiagnosticList::HasErrors() const
{
    for (const auto& diagnostic : m_items)
    {
        if (diagnostic.IsError())
            return true;
    }
    return false;
}

const std::vector<ImportDiagnostic>& ImportDiagnosticList::GetItems() const
{
    return m_items;
}

bool ImportDiagnosticList::Empty() const
{
    return m_items.empty();
}
}
