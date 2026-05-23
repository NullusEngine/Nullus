#pragma once

#include "Assets/ArtifactManifest.h"
#include "Assets/ImportDiagnostics.h"
#include "CoreDef.h"

#include <unordered_map>

namespace NLS::Core::Assets
{
enum class AssetResolverState
{
    Missing,
    Importing,
    Failed,
    NeedsImport,
    UpToDate
};

class NLS_CORE_API AssetResolver
{
public:
    void MarkImporting(AssetId asset);
    void MarkNeedsImport(AssetId asset);
    void CommitSuccessfulImport(ArtifactManifest manifest);
    void CommitFailedImport(AssetId asset, ImportDiagnosticList diagnostics);

    AssetResolverState GetState(AssetId asset) const;
    const ArtifactManifest* GetCommittedManifest(AssetId asset) const;
    const ImportDiagnosticList* GetDiagnostics(AssetId asset) const;

private:
    std::unordered_map<AssetId, AssetResolverState> m_states;
    std::unordered_map<AssetId, ArtifactManifest> m_committedManifests;
    std::unordered_map<AssetId, ImportDiagnosticList> m_diagnostics;
};
}
