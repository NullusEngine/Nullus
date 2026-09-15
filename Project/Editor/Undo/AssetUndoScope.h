#pragma once

#include <string>

#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Undo
{
// Asset Browser, importers, and Inspector targets must use the same durable
// identity. The caller supplies a canonical project resource path (Assets/...)
// and, when available, the GUID stored in its .meta companion.
[[nodiscard]] inline UndoScopeId MakeAssetUndoScope(
    const std::string& resourcePath,
    const std::string& assetId = {})
{
    if (!assetId.empty())
        return {UndoScopeKind::Asset, "asset:" + assetId, 0u};
    return {UndoScopeKind::Asset,
        resourcePath.empty() ? std::string {} : "path:" + resourcePath,
        0u};
}
} // namespace NLS::Editor::Undo
