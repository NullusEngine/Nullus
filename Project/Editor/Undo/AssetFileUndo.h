#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <Assets/AssetId.h>
#include "Rendering/Assets/MaterialAsset.h"

#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Undo::AssetFile
{
// A project mutation owns the complete source tree and every serialized
// reference file it changes. An empty destination denotes a delete; otherwise
// the source is renamed or moved. Both endpoints must live below
// <projectRoot>/Assets.
//
// This is deliberately separate from the single-file helpers below. A folder
// or a reference-bearing source asset needs a cross-scope, exact byte snapshot
// so it can never fall back to EditorActions' legacy direct propagation path.
[[nodiscard]] UndoCommandPtr MakeProjectAssetMutationCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath = {},
    std::string assetIdentifier = {},
    std::string label = {});

// Creates a source file and a stable .meta companion. Supplying assetId keeps
// a cross-scope command's asset identity aligned with its serialized payload;
// otherwise a fresh ID is allocated. Redo always preserves that initial ID.
[[nodiscard]] UndoCommandPtr MakeCreateFileCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& destinationPath,
    std::string contents,
    std::string assetIdentifier = {},
    std::string label = "Create Asset",
    NLS::Core::Assets::AssetId assetId = {});

[[nodiscard]] UndoCommandPtr MakeCreateMaterialCommand(
    const std::filesystem::path& projectRoot,const std::filesystem::path& destinationPath,
    NLS::Render::Assets::MaterialAsset asset,std::string label = "Create Material");

// Creates one directory. Undo succeeds only while it is still empty, avoiding
// an unsafe recursive delete of content created outside the Undo timeline.
[[nodiscard]] UndoCommandPtr MakeCreateDirectoryCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& directoryPath,
    std::string assetIdentifier = {},
    std::string label = "Create Folder");

// A regular source file and its optional .meta companion are one undo unit.
// This deliberately excludes directories: directory moves/deletes also carry
// project reference propagation that must be modelled as a transaction.
[[nodiscard]] UndoCommandPtr MakeDuplicateCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    std::string assetIdentifier = {});

// Renames a regular source file and its optional .meta companion as a single
// reversible unit. Directory moves intentionally remain outside this command:
// they need a cross-scope reference propagation transaction.
[[nodiscard]] UndoCommandPtr MakeRenameCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    std::string assetIdentifier = {});

// Deletes a regular source file by moving the file and its optional .meta
// companion to the per-command UndoTrash location. The command refuses to
// overwrite a path recreated outside the timeline during Undo.
[[nodiscard]] UndoCommandPtr MakeDeleteCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    std::string assetIdentifier = {});
} // namespace NLS::Editor::Undo::AssetFile
