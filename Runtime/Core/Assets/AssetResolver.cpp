#include "Assets/AssetResolver.h"

namespace NLS::Core::Assets
{
void AssetResolver::MarkImporting(AssetId asset)
{
    m_states[asset] = AssetResolverState::Importing;
}

void AssetResolver::MarkNeedsImport(AssetId asset)
{
    m_states[asset] = AssetResolverState::NeedsImport;
}

void AssetResolver::CommitSuccessfulImport(ArtifactManifest manifest)
{
    const auto asset = manifest.sourceAssetId;
    m_committedManifests[asset] = std::move(manifest);
    m_diagnostics.erase(asset);
    m_states[asset] = AssetResolverState::UpToDate;
}

void AssetResolver::CommitFailedImport(AssetId asset, ImportDiagnosticList diagnostics)
{
    m_diagnostics[asset] = std::move(diagnostics);
    m_states[asset] = AssetResolverState::Failed;
}

AssetResolverState AssetResolver::GetState(AssetId asset) const
{
    const auto found = m_states.find(asset);
    if (found != m_states.end())
        return found->second;
    return m_committedManifests.find(asset) != m_committedManifests.end()
        ? AssetResolverState::UpToDate
        : AssetResolverState::Missing;
}

const ArtifactManifest* AssetResolver::GetCommittedManifest(AssetId asset) const
{
    const auto found = m_committedManifests.find(asset);
    if (found == m_committedManifests.end())
        return nullptr;
    return &found->second;
}

const ImportDiagnosticList* AssetResolver::GetDiagnostics(AssetId asset) const
{
    const auto found = m_diagnostics.find(asset);
    if (found == m_diagnostics.end())
        return nullptr;
    return &found->second;
}
}
