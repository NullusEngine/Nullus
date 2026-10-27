// Hand-written declarations for IconBaker-generated tables.
#pragma once

#include <vector>

#include "Core/EditorResourceCatalog.h"
#include "Icons/EditorIconCatalog.h"

namespace NLS::Editor::Icons
{
    const std::vector<IconDefinition>& GeneratedIconDefinitions();
}

namespace NLS::Editor::Core
{
    std::vector<EditorResourceRecord> GeneratedIconRecords();
}
