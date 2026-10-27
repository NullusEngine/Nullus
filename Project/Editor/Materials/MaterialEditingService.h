#pragma once
#include <future>
#include <map>
#include <set>
#include "MaterialDocument.h"
#include "MaterialSourceDocument.h"
#include "MaterialCompiler/MaterialShaderCompiler.h"
#include "MaterialCompiler/MaterialCooker.h"
#include "Rendering/Resources/Loaders/MaterialLoader.h"

namespace NLS::Editor::Assets {class AssetDatabaseFacade;}
namespace NLS::Editor::Materials
{
struct CompiledMaterialEdit
{
    struct Dependent
    {
        Render::Resources::Material* target = nullptr;
        uint64_t instanceId = 0;
        Render::Assets::MaterialAsset asset;
        Render::Assets::ResolvedMaterialAsset resolved;
    };
    Render::Assets::MaterialAsset asset;
    std::string documentSource;
    Render::Assets::ResolvedMaterialAsset resolved;
    std::vector<Render::Assets::ShaderArtifact> passes;
    MaterialCompiler::MaterialDependencySnapshot dependencies;
    std::string error;
    bool publication = false;
    std::vector<Dependent> dependents;
    std::map<std::string,MaterialCompiler::MaterialCookTexture> textures;
};

struct MaterialEditSession
{
    explicit MaterialEditSession(Undo::EditorUndoService* undo):document(undo) {}
    MaterialDocument document;
    Render::Resources::Material* sceneMaterial = nullptr;
    uint64_t sceneMaterialId = 0;
    std::shared_ptr<Render::Resources::Material> preview;
    std::unique_ptr<Render::Resources::Material> draft;
    std::optional<CompiledMaterialEdit> compiled;
    std::future<CompiledMaterialEdit> pending;
    std::string requestedSource;
    std::string failedSource;
    std::string error;
    std::set<std::string> workingDependencies;
    std::map<std::string,Render::Resources::MaterialParameterValue> previewUpdates;
    bool applyRequested = false;
    bool preparePublication = false;
    bool sourcePreview = false;
    bool publishShaderFamily = false;
    uint64_t appliedGeneration = 0;
    size_t variantBudget=128;
    std::future<MaterialCompiler::MaterialCookResult> pendingCook;
    std::filesystem::path lastRelease;
};

struct MaterialSourceEditSession
{
    explicit MaterialSourceEditSession(Undo::EditorUndoService* undo):document(undo){}
    SourceTools::MaterialSourceDocument document;
    std::shared_ptr<MaterialEditSession> preview;
    std::string appliedSource;
    std::string appliedSnapshot;
    std::string error;
};

// Editor-owned service. UI and Inspector share sessions, history and publication.
class MaterialEditingService final
{
public:
    MaterialEditingService(std::filesystem::path projectRoot,std::filesystem::path engineRoot,
        Undo::EditorUndoService* undo = nullptr);
    ~MaterialEditingService();
    std::shared_ptr<MaterialEditSession> Open(const std::filesystem::path& path,std::string& error);
    std::shared_ptr<MaterialEditSession> Find(const std::filesystem::path& path) const;
    std::shared_ptr<MaterialSourceEditSession> OpenSource(const std::filesystem::path& path,std::string& error);
    std::string ReadSource(const std::filesystem::path& path,bool working) const;
    bool PreviewSource(MaterialSourceEditSession& session);
    bool ApplySource(MaterialSourceEditSession& session);
    const auto& SourceSessions() const {return m_sourceSessions;}
    std::shared_ptr<MaterialEditSession> OpenTarget(std::string_view assetId,std::string_view resourcePath,std::string& error);
    std::shared_ptr<MaterialEditSession> FindDraft(const Render::Resources::Material* material) const;
    bool CaptureInspectorChanges(MaterialEditSession& session,const Render::Resources::Material& before);
    bool RequestPreview(MaterialEditSession& session);
    void Pump();
    bool Save(MaterialEditSession& session);
    bool ApplyToScene(MaterialEditSession& session);
    bool PublishResources(MaterialEditSession& session);
    bool SetParameter(MaterialEditSession& session,const Render::Resources::MaterialParameterDesc& parameter,
        Render::Resources::MaterialParameterValue value);
    bool ClearParameter(MaterialEditSession& session,Render::Resources::MaterialParameterId id,std::string_view name);
    std::filesystem::path ResolvePath(std::string_view path) const;
    Render::Assets::MaterialAssetReference ResolveReference(std::string path,std::string subAsset = {}) const;
    const std::map<std::string,std::shared_ptr<MaterialEditSession>>& Sessions() const {return m_sessions;}

private:
    std::optional<Render::Assets::MaterialAsset> ReadAsset(const Render::Assets::MaterialAssetReference& reference,
        bool working,std::string& error) const;
    bool Current(const CompiledMaterialEdit& result,const MaterialEditSession& session) const;
    bool Publish(MaterialEditSession& session);
    void RestoreAppliedSources(const Render::Resources::Shader* shader);
    std::unique_ptr<Render::Resources::Material> BuildRuntime(const CompiledMaterialEdit& result,std::string& error);
    std::filesystem::path m_projectRoot,m_engineRoot;
    Undo::EditorUndoService* m_undo = nullptr;
    std::unique_ptr<Assets::AssetDatabaseFacade> m_database;
    std::map<std::string,std::shared_ptr<MaterialEditSession>> m_sessions;
    std::map<std::string,std::shared_ptr<MaterialSourceEditSession>> m_sourceSessions;
    std::map<std::string,MaterialCompiler::MaterialDependencySource> m_appliedSources;
    uint64_t m_publication = 1;
};
}
