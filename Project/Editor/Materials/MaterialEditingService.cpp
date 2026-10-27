#include "MaterialEditingService.h"

#include <fstream>
#include <regex>
#include <algorithm>
#include <chrono>
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"
#include "MaterialCompiler/MaterialGraph.h"
#include "Rendering/Resources/MaterialInstance.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Math/Matrix4.h"
#include "Inspector/InspectorUndo.h"

namespace NLS::Editor::Materials
{
namespace
{
std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path,std::ios::binary);
    return input?std::string{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()}:std::string{};
}
std::string PathKey(const std::filesystem::path& path)
{ return std::filesystem::absolute(path).lexically_normal().generic_string(); }
std::string Hash(std::string_view text)
{return NLS::Core::Assets::ComputeNativeArtifactPayloadHash(reinterpret_cast<const uint8_t*>(text.data()),text.size());}
}

MaterialEditingService::MaterialEditingService(std::filesystem::path projectRoot,std::filesystem::path engineRoot,
    Undo::EditorUndoService* undo):m_projectRoot(std::move(projectRoot)),m_engineRoot(std::move(engineRoot)),m_undo(undo)
{
    m_database=std::make_unique<Assets::AssetDatabaseFacade>(Assets::MakeProjectEditorAssetRoots(m_projectRoot));
    NLS::Core::ServiceLocator::Provide(*this);
}
MaterialEditingService::~MaterialEditingService()
{
    for (auto& [key,session] : m_sessions) if (session->pending.valid()) session->pending.wait();
    for (auto& [key,session] : m_sessions) if (session->pendingCook.valid()) session->pendingCook.wait();
    if (auto* driver=Render::Context::TryGetLocatedDriver()) Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);
    NLS::Core::ServiceLocator::Remove<MaterialEditingService>();
}

std::filesystem::path MaterialEditingService::ResolvePath(std::string_view path) const
{
    const std::filesystem::path supplied(path);
    if (supplied.is_absolute()) return supplied.lexically_normal();
    const std::string text(path);
    if (text.starts_with("Assets/Engine/")) return (m_engineRoot/text.substr(14)).lexically_normal();
    if (text.starts_with(":Shaders/")) return (m_engineRoot/text.substr(1)).lexically_normal();
    return (m_projectRoot/supplied).lexically_normal();
}

Render::Assets::MaterialAssetReference MaterialEditingService::ResolveReference(std::string path,std::string subAsset) const
{
    std::replace(path.begin(),path.end(),'\\','/');
    if (!path.empty() && !path.starts_with("Assets/") && !std::filesystem::path(path).is_absolute()) path="Assets/"+path;
    return {m_database->AssetPathToGUID(path),std::move(subAsset),std::move(path)};
}

std::shared_ptr<MaterialEditSession> MaterialEditingService::Find(const std::filesystem::path& path) const
{
    const auto it=m_sessions.find(PathKey(path));
    return it==m_sessions.end()?nullptr:it->second;
}

std::string MaterialEditingService::ReadSource(const std::filesystem::path& path,bool working) const
{
    const auto key=PathKey(path);
    if(working)
        if(const auto found=m_sourceSessions.find(key);found!=m_sourceSessions.end())return found->second->document.Asset();
    if(!working)
        if(const auto found=m_appliedSources.find("file:"+key);found!=m_appliedSources.end())return found->second.content;
    return ReadText(path);
}

std::shared_ptr<MaterialSourceEditSession> MaterialEditingService::OpenSource(const std::filesystem::path& path,std::string& error)
{
    const auto key=PathKey(path);
    if(const auto found=m_sourceSessions.find(key);found!=m_sourceSessions.end())return found->second;
    const auto extension=path.extension();
    if(extension!=".shader" && extension!=".shadergraph" && extension!=".shaderfunc")
    {error="Open a ShaderLab, ShaderGraph or material function asset";return {};}
    auto session=std::make_shared<MaterialSourceEditSession>(m_undo);
    if(!session->document.Load(path)){error=session->document.Error();return {};}
    bool loadedSource=false;
    if(NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
        for(const auto& [key,material]:NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResources())
            if(material){RestoreAppliedSources(material->GetShader());
                if(material->GetShader() && PathKey(ResolvePath(material->GetShader()->GetImportedArtifactSourcePath()))==PathKey(path))loadedSource=true;}
    const auto identity="file:"+key;
    if(!loadedSource || m_appliedSources.contains(identity))
    {session->appliedSource=ReadSource(path,false);
        m_appliedSources.try_emplace(identity,MaterialCompiler::MaterialDependencySource{identity,path,
            session->appliedSource,Hash(ReadText(NLS::Core::Assets::GetAssetMetaPath(path))),m_publication,{}});}
    if(extension!=".shaderfunc")
    {
        auto preview=std::make_shared<MaterialEditSession>(m_undo);
        Render::Assets::MaterialAsset material;
        material.identity.path="preview:"+key;
        material.shader.path=key;
        preview->document.New(material);
        preview->sourcePreview=true;
        preview->workingDependencies.insert(identity);
        session->preview=preview;
        m_sessions.emplace("preview:"+key,preview);
    }
    m_sourceSessions.emplace(key,session);
    error.clear();return session;
}

bool MaterialEditingService::PreviewSource(MaterialSourceEditSession& session)
{
    if(!session.preview){session.error="Preview this function through a material graph that calls it";return false;}
    session.preview->failedSource.clear();
    const auto result=RequestPreview(*session.preview);session.error=session.preview->error;return result;
}

bool MaterialEditingService::ApplySource(MaterialSourceEditSession& session)
{
    if(!session.document.EndEdit()) {session.error=session.document.Error();return false;}
    if(!session.preview){session.error="Apply a function as an explicit Working dependency of its calling material";return false;}
    session.preview->publishShaderFamily=true;
    return ApplyToScene(*session.preview);
}

std::shared_ptr<MaterialEditSession> MaterialEditingService::Open(const std::filesystem::path& path,std::string& error)
{
    if (auto existing=Find(path)) return existing;
    m_database->Refresh();
    auto session=std::make_shared<MaterialEditSession>(m_undo);
    if (!session->document.Load(path)) {error=session->document.Error();return {};}
    session->document.SetUndoScope(Inspector::MakeInspectorAssetUndoScope(
        session->document.Asset().identity.path,session->document.Asset().identity.guid));
    if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
    {
        auto& manager=NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>();
        session->sceneMaterial=manager.GetResource(session->document.Asset().identity.path);
        if (session->sceneMaterial) session->sceneMaterialId=session->sceneMaterial->GetInstanceId();
    }
    if(session->sceneMaterial)RestoreAppliedSources(session->sceneMaterial->GetShader());
    const auto appliedSource=session->sceneMaterial && session->sceneMaterial->GetAssetData()
        ?Render::Assets::SerializeMaterialAsset(*session->sceneMaterial->GetAssetData()):session->document.Source();
    session->document.MarkApplied("initial",appliedSource);
    const auto identity="material:"+session->document.Asset().identity.Key();
    m_appliedSources.try_emplace(identity,MaterialCompiler::MaterialDependencySource{identity,path,
        appliedSource,{},m_publication,{}});
    m_sessions.emplace(PathKey(path),session);
    if(session->sceneMaterial && session->sceneMaterial->GetShader())
    {
        session->draft=session->sceneMaterial->CreateDynamicCopy();
        session->preview=std::shared_ptr<Render::Resources::Material>(session->sceneMaterial->CreateDynamicCopy().release());
    }
    RequestPreview(*session);
    error.clear();return session;
}

void MaterialEditingService::RestoreAppliedSources(const Render::Resources::Shader* shader)
{
    if(!shader || shader->GetDependencySnapshotId().empty())return;
    auto id=shader->GetDependencySnapshotId();id=id.substr(id.find(':')+1);
    std::vector<std::filesystem::path> roots={m_projectRoot/"Library"/"MaterialEditor"/"Generated",
        m_projectRoot/"Library"/"ShaderCache"/"ImportedShaderLab",
        m_projectRoot/"Library"/"Engine"/"ShaderCache"/"ImportedShaderLab",
        m_engineRoot.parent_path()/"Library"/"ShaderCache"/"ImportedShaderLab"};
    for(auto directory=std::filesystem::path(shader->path).parent_path();!directory.empty() && directory!=directory.parent_path();directory=directory.parent_path())
        if(directory.filename()=="Library")
        {roots.push_back(directory/"ShaderCache"/"ImportedShaderLab");roots.push_back(directory/"MaterialEditor"/"Generated");break;}
    for(const auto& root:roots)
    {
        std::string error;
        const auto snapshot=MaterialCompiler::MaterialDependencySnapshot::Load(root/"MaterialSnapshots"/(id+".json"),error);
        if(!snapshot)continue;
        for(const auto& [key,source]:snapshot->sources)
        {
            if(source.path.empty() || source.path.extension()==".mat" || source.path.generic_string().find("MaterialEditor/Generated")!=std::string::npos ||
                source.path.generic_string().find("ImportedShaderLab/")!=std::string::npos)continue;
            auto applied=source;applied.identity="file:"+PathKey(source.path);
            if(!applied.appliedGeneration)applied.appliedGeneration=m_publication;
            m_appliedSources.try_emplace(applied.identity,std::move(applied));
        }
        return;
    }
}

std::shared_ptr<MaterialEditSession> MaterialEditingService::OpenTarget(std::string_view assetId,std::string_view resourcePath,std::string& error)
{
    for (const auto& [key,session] : m_sessions)
        if ((!assetId.empty() && session->document.Asset().identity.guid==assetId) ||
            session->document.Asset().identity.path==resourcePath) return session;
    m_database->Refresh();
    auto path=assetId.empty()?std::string(resourcePath):m_database->GUIDToAssetPath(std::string(assetId));
    if (path.empty()) path=resourcePath;
    if (std::filesystem::path(path).extension()!=".mat") return {};
    if (!path.starts_with("Assets/") && !std::filesystem::path(path).is_absolute()) path="Assets/"+path;
    const auto physical=ResolvePath(path);
    if (!std::filesystem::is_regular_file(physical)) return {};
    return Open(physical,error);
}

std::shared_ptr<MaterialEditSession> MaterialEditingService::FindDraft(const Render::Resources::Material* material) const
{
    for (const auto& [key,session] : m_sessions) if (session->draft.get()==material) return session;
    return {};
}

std::optional<Render::Assets::MaterialAsset> MaterialEditingService::ReadAsset(
    const Render::Assets::MaterialAssetReference& reference,bool working,std::string& error) const
{
    const auto identity="material:"+reference.Key();
    if (!working)
        if (const auto applied=m_appliedSources.find(identity);applied!=m_appliedSources.end())
            return Render::Assets::DeserializeMaterialAsset(applied->second.content,error);
    if(!working && NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
        for(const auto& [key,material]:NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResources())
            if(material && !material->IsDynamicInstance() && material->GetAssetData() &&
                material->GetAssetData()->identity.Key()==reference.Key())return *material->GetAssetData();
    const auto hint=reference.guid.empty()?reference.path:m_database->GUIDToAssetPath(reference.guid);
    const auto path=ResolvePath(hint);
    if (auto session=Find(path))
    {
        const auto& source=working?session->document.Source():session->document.AppliedSource();
        if (!source.empty()) return Render::Assets::DeserializeMaterialAsset(source,error);
    }
    auto physical=path;
    if (physical.extension()!=".mat") physical=m_database->ResolveArtifactPathAtPath(hint,reference.subAssetKey);
    std::ifstream input(physical,std::ios::binary);
    if (!input) {error="Cannot read parent material: "+reference.Key();return std::nullopt;}
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    const auto container=NLS::Core::Assets::ReadNativeArtifactContainer(bytes,NLS::Core::Assets::ArtifactType::Material,1);
    if (!container) {error="Parent material is not a native material asset";return std::nullopt;}
    auto asset=Render::Assets::DeserializeMaterialAsset(std::string_view(reinterpret_cast<const char*>(container->payload.data()),container->payload.size()),error);
    if (asset) asset->identity=reference;
    return asset;
}

bool MaterialEditingService::RequestPreview(MaterialEditSession& session)
{
    if (session.pending.valid()) return true;
    if (session.document.Source()==session.failedSource && !session.sourcePreview) return false;
    if (!session.preparePublication && session.compiled && session.compiled->documentSource==session.document.Source() && Current(*session.compiled,session)) return true;
    CompiledMaterialEdit input;input.documentSource=session.document.Source();
    std::string error;
    auto asset=session.document.Asset();
    input.asset=asset;
    std::map<std::string,MaterialCompiler::MaterialDependencySource> sources;
    const auto rootKey="material:"+asset.identity.Key();
    sources[rootKey]={rootKey,session.document.Path(),input.documentSource,{},0,{}};
    auto provider=[&](const Render::Assets::MaterialAssetReference& reference)->std::optional<Render::Assets::MaterialAsset>
    {
        const auto key="material:"+reference.Key();
        const auto parent=ReadAsset(reference,session.workingDependencies.contains(key),error);
        if (parent)
        {
            const auto path=reference.guid.empty()?reference.path:m_database->GUIDToAssetPath(reference.guid);
            sources[key]={key,ResolvePath(path),Render::Assets::SerializeMaterialAsset(*parent),{},
                m_appliedSources.contains(key)?m_appliedSources.at(key).appliedGeneration:0,{}};
            if (!session.workingDependencies.contains(key) && !m_appliedSources.contains(key))
            {sources[key].appliedGeneration=m_publication;m_appliedSources.emplace(key,sources[key]);}
        }
        return parent;
    };
    auto resolved=Render::Assets::MaterialAssetResolver::Resolve(asset,provider,error);
    if (!resolved) {session.error=error;session.failedSource=input.documentSource;return false;}
    input.resolved=*resolved;
    input.publication=session.preparePublication;
    if (input.publication && NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
    {
        const auto rootIdentity=asset.identity.Key();
        for (const auto& [key,material] : NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResources())
        {
            if (!material || material->IsDynamicInstance() || !material->GetAssetData() ||
                material->GetAssetData()->identity.Key()==rootIdentity) continue;
            const auto child=*material->GetAssetData();
            const auto childProvider=[&](const Render::Assets::MaterialAssetReference& ref)->std::optional<Render::Assets::MaterialAsset>
            {return ref.Key()==rootIdentity?std::optional{asset}:provider(ref);};
            auto childResolved=Render::Assets::MaterialAssetResolver::Resolve(child,childProvider,error);
            if (!childResolved) {session.error=error;return false;}
            const bool descendant=std::any_of(childResolved->ancestry.begin(),childResolved->ancestry.end(),
                [&](const auto& ref){return ref.Key()==rootIdentity;});
            const bool sameShader=session.publishShaderFamily &&
                PathKey(ResolvePath(childResolved->shader.path))==PathKey(ResolvePath(input.resolved.shader.path));
            if(!descendant && !sameShader)continue;
            if (std::any_of(input.dependents.begin(),input.dependents.end(),
                [&](const auto& entry){return entry.target==material;})) continue;
            input.dependents.push_back({material,material->GetInstanceId(),child,*childResolved});
            const auto dependency="material:"+child.identity.Key();
            sources[dependency]={dependency,ResolvePath(child.identity.path),Render::Assets::SerializeMaterialAsset(child),{},
                m_appliedSources.contains(dependency)?m_appliedSources.at(dependency).appliedGeneration:0,{}};
            if (!m_appliedSources.contains(dependency))
            {sources[dependency].appliedGeneration=m_publication;m_appliedSources.emplace(dependency,sources[dependency]);}
        }
    }
    if (!resolved->shader.guid.empty()) input.resolved.shader.path=m_database->GUIDToAssetPath(resolved->shader.guid);
    for(auto& dependent:input.dependents)dependent.resolved.shader=input.resolved.shader;
    if(session.preview && session.preview->GetShader())
        for(const auto& parameter:session.preview->GetShader()->GetMaterialParameterSchema()->parameters)
            if(parameter.isStatic && std::holds_alternative<bool>(parameter.defaultValue))
                input.resolved.staticParameters.try_emplace(parameter.name,std::get<bool>(parameter.defaultValue));
    const auto shaderPath=ResolvePath(input.resolved.shader.path);
    if(session.sceneMaterial)RestoreAppliedSources(session.sceneMaterial->GetShader());
    const auto shaderKey="file:"+PathKey(shaderPath);
    if(session.sceneMaterial && session.sceneMaterial->GetShader() && !m_appliedSources.contains(shaderKey) &&
        !session.workingDependencies.contains(shaderKey))
    {session.error="The loaded shader has no Applied source archive. Select its current source explicitly before recompiling.";return false;}
    MaterialCompiler::MaterialDependencySource shaderSource;
    if (const auto applied=m_appliedSources.find(shaderKey);
        applied!=m_appliedSources.end() && !session.workingDependencies.contains(shaderKey)) shaderSource=applied->second;
    else shaderSource={shaderKey,shaderPath,ReadSource(shaderPath,session.workingDependencies.contains(shaderKey)),Hash(ReadText(NLS::Core::Assets::GetAssetMetaPath(shaderPath))),0,{}};
    if (shaderSource.content.empty()) {session.error="Shader source is missing: "+shaderPath.string();return false;}
    if (!session.workingDependencies.contains(shaderKey) && !m_appliedSources.contains(shaderKey))
    {shaderSource.appliedGeneration=m_publication;m_appliedSources.emplace(shaderKey,shaderSource);}
    sources[shaderKey]=shaderSource;
    Render::Resources::MaterialParameterSchema sourceSchema;
    if(shaderPath.extension()==".shadergraph")
    {
        MaterialCompiler::MaterialGraphAsset graph;std::string parseError;
        if(MaterialCompiler::DeserializeMaterialGraph(shaderSource.content,graph,parseError))
            for(const auto& parameter:graph.parameters)
                if(parameter.type==MaterialCompiler::MaterialGraphType::Texture2D || parameter.type==MaterialCompiler::MaterialGraphType::TextureCube)
                {Render::Resources::MaterialParameterDesc desc;desc.id=parameter.id;desc.name=MaterialCompiler::MaterialGraphBindingName(parameter.id);
                    desc.type=parameter.type==MaterialCompiler::MaterialGraphType::Texture2D?Render::Resources::UniformType::UNIFORM_SAMPLER_2D:Render::Resources::UniformType::UNIFORM_SAMPLER_CUBE;
                    desc.defaultValue=parameter.value;sourceSchema.parameters.push_back(std::move(desc));}
    }
    else
    {const auto parsed=Render::ShaderLab::ParseShaderLabSource(shaderSource.content,shaderPath.generic_string());
        const auto meta=NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(shaderPath));
        if(parsed.Succeeded())sourceSchema=MaterialCompiler::BuildMaterialParameterSchema(parsed.asset,meta?meta->id.ToString():input.resolved.shader.path);}
    const auto captureTextures=[&](auto& settings,const Render::Resources::Material* live)->bool
    {
        using U=Render::Resources::UniformType;
        std::optional<Render::Assets::ResolvedMaterialAsset> previous;
        const auto liveSchema=live && Render::Resources::Material::IsLive(live) && live->GetShader()?live->GetShader()->GetMaterialParameterSchema():nullptr;
        if(liveSchema && live->GetAssetData())
        {std::string previousError;previous=Render::Assets::MaterialAssetResolver::Resolve(*live->GetAssetData(),
            [&](const auto& reference){return ReadAsset(reference,false,previousError);},previousError,liveSchema.get());}
        for(const auto& parameter:sourceSchema.parameters)
            if(parameter.type==U::UNIFORM_SAMPLER_2D || parameter.type==U::UNIFORM_SAMPLER_CUBE)
                if(std::none_of(settings.parameters.begin(),settings.parameters.end(),[&](const auto& item){return item.second.id==parameter.id || item.first==parameter.name;}))
                    settings.parameters.try_emplace(parameter.name,Render::Assets::MaterialParameterOverride{parameter.id,parameter.name,parameter.type,parameter.defaultValue});
        for(const auto& [name,parameter]:settings.parameters)
        {
            if(parameter.type!=U::UNIFORM_SAMPLER_2D && parameter.type!=U::UNIFORM_SAMPLER_CUBE)continue;
            const auto* path=std::get_if<std::string>(&parameter.value);
            if(!path || path->empty() || *path=="white" || *path=="black" || *path=="bump")continue;
            const auto key="texture:"+*path;
            MaterialCompiler::MaterialDependencySource frozen;
            if(const auto applied=m_appliedSources.find(key);applied!=m_appliedSources.end() && !session.workingDependencies.contains(key))frozen=applied->second;
            else
            {
                auto physical=ResolvePath(*path);
                bool loadedArtifact=false;
                const auto* oldDefinition=liveSchema?(parameter.id?liveSchema->Find(parameter.id):liveSchema->Find(name)):nullptr;
                const auto old=previous && oldDefinition?previous->parameters.find(oldDefinition->name):settings.parameters.end();
                if(!session.workingDependencies.contains(key) && previous && oldDefinition && old!=previous->parameters.end() &&
                    old->second.value==parameter.value)
                {
                    const auto appliedPath=ResolvePath(live->GetTextureResourcePath(name));
                    if(!live->GetTextureResourcePath(name).empty())
                    {const auto bytes=ReadText(appliedPath);
                        if(Render::Assets::DeserializeTextureArtifact(std::vector<uint8_t>(bytes.begin(),bytes.end())))
                        {physical=appliedPath;loadedArtifact=true;}}
                }
                if(!loadedArtifact)
                    for(const auto& entry:m_database->LoadAllAssetsAtPath(*path))if(entry.artifactType==NLS::Core::Assets::ArtifactType::Texture)
                    {physical=m_database->ResolveArtifactPathAtPath(*path,entry.subAssetKey);break;}
                frozen={key,physical,ReadText(physical),{},0,{}};
            }
            const std::vector<uint8_t> bytes(frozen.content.begin(),frozen.content.end());
            if(!Render::Assets::DeserializeTextureArtifact(bytes))
            {session.error="Material texture needs an imported native artifact: "+*path;return false;}
            const auto native=NLS::Core::Assets::ReadNativeArtifactContainer(bytes,NLS::Core::Assets::ArtifactType::Texture,4);
            if(!native){session.error="Material texture snapshot is invalid: "+*path;return false;}
            input.textures[*path]={native->metadata.sourceAssetId,native->metadata.subAssetKey,bytes};
            if(!session.workingDependencies.contains(key) && !m_appliedSources.contains(key))
            {frozen.appliedGeneration=m_publication;m_appliedSources.emplace(key,frozen);}
            sources[key]=std::move(frozen);
        }
        return true;
    };
    if(!captureTextures(input.resolved,session.sceneMaterial))return false;
    for(auto& child:input.dependents)if(!captureTextures(child.resolved,child.target))return false;
    if (session.compiled && session.compiled->resolved.shader==input.resolved.shader &&
        session.compiled->resolved.staticParameters==input.resolved.staticParameters &&
        session.compiled->resolved.ancestry==input.resolved.ancestry)
    {
        auto reused=*session.compiled;
            reused.asset=asset;reused.documentSource=input.documentSource;reused.resolved=input.resolved;
            reused.publication=input.publication;reused.dependents=input.dependents;reused.textures=input.textures;
        bool variantsAvailable=true;
        for (const auto& [key,source] : sources)
            if (key.starts_with("material:") || key.starts_with("texture:")) reused.dependencies.sources[key]=source;
            else
            {
                const auto old=reused.dependencies.sources.find(key);
                if(old==reused.dependencies.sources.end() || old->second.content!=source.content ||
                    old->second.metadataHash!=source.metadataHash)variantsAvailable=false;
            }
        auto checkVariant=[&](const auto& settings)
        {
            Render::ShaderLab::ShaderLabKeywordSet keywords;
            for(const auto& [name,value]:settings)if(value)keywords.Enable(name);
            for(const auto& pass:reused.passes)
                if(std::none_of(pass.variantReflections.begin(),pass.variantReflections.end(),
                    [&](const auto& variant){return variant.keywordHash==keywords.Hash();}))variantsAvailable=false;
        };
        checkVariant(reused.resolved.staticParameters);
        for(const auto& dependent:reused.dependents)checkVariant(dependent.resolved.staticParameters);
        if (variantsAvailable && Current(reused,session))
        {
            std::promise<CompiledMaterialEdit> ready;
            session.pending=ready.get_future();ready.set_value(std::move(reused));
            session.failedSource.clear();session.error.clear();return true;
        }
    }
    std::map<std::string,MaterialCompiler::MaterialDependencySource> sourceOverrides;
    const auto usesWorking=[&](const auto& key,const auto& source)
        {return session.workingDependencies.contains(key) || (!source.path.empty() &&
            (session.workingDependencies.contains("file:"+PathKey(source.path)) || session.workingDependencies.contains(PathKey(source.path))));};
    for (const auto& [key,source] : m_appliedSources)
        if (key.starts_with("file:") && !usesWorking(key,source)) sourceOverrides.try_emplace(key,source);
    for(const auto& [key,source]:m_sourceSessions)
        if(session.workingDependencies.contains("file:"+key) || session.workingDependencies.contains(key))
            sourceOverrides["file:"+key]={"file:"+key,source->document.Path(),source->document.Asset(),
                Hash(ReadText(NLS::Core::Assets::GetAssetMetaPath(source->document.Path()))),0,{}};
    const auto shaderSourceText=shaderSource.content;
    const auto shaderMeta=NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(shaderPath));
    const auto shaderOwner=shaderMeta?shaderMeta->id.ToString():input.resolved.shader.path;
    std::string context="material-preview";
    const auto device=Render::Context::TryGetLocatedActiveGraphicsBackend();
    const auto target=device && (*device==Render::Settings::EGraphicsBackend::VULKAN || *device==Render::Settings::EGraphicsBackend::METAL)
        ?Render::ShaderCompiler::ShaderTargetPlatform::SPIRV:Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    input.dependencies.context=context;
    input.dependencies.sources=std::move(sources);
    session.requestedSource=input.documentSource;
    const auto cache=m_projectRoot/"Library"/"MaterialEditor";
    const auto shaderIdentity=input.resolved.shader.path;
    const auto engineShaders=m_engineRoot/"Shaders";
    const auto projectRoot=m_projectRoot;
    const auto database=Assets::AssetDatabaseFacade::CreateReadOnlySnapshot(*m_database);
    const auto variantBudget=session.variantBudget;
    session.pending=std::async(std::launch::async,[input=std::move(input),variantBudget,sourceOverrides=std::move(sourceOverrides),shaderPath,shaderSourceText,shaderIdentity,shaderOwner,cache,engineShaders,target,projectRoot,database]() mutable
    {
        Render::ShaderLab::ShaderLabAssetDesc shader;
        Render::Resources::MaterialParameterSchema schema;
        if(shaderPath.extension()==".shadergraph")
        {
            MaterialCompiler::MaterialGraphAsset graph;
            if(!MaterialCompiler::DeserializeMaterialGraph(shaderSourceText,graph,input.error))return input;
            const auto functions=[&](const Render::Assets::MaterialAssetReference& reference)->std::optional<MaterialCompiler::MaterialGraphAsset>
            {
                const auto hint=reference.guid.empty()?reference.path:database->GUIDToAssetPath(reference.guid);
                const std::filesystem::path path=hint;
                auto physical=path.is_absolute()?path:projectRoot/path;
                if(!std::filesystem::exists(physical))physical=shaderPath.parent_path()/path;
                const auto applied=std::find_if(sourceOverrides.begin(),sourceOverrides.end(),
                    [&](const auto& source){return !source.second.path.empty() && PathKey(source.second.path)==PathKey(physical);});
                MaterialCompiler::MaterialGraphAsset function;
                const auto text=applied==sourceOverrides.end()?ReadText(physical):applied->second.content;
                if(!MaterialCompiler::DeserializeMaterialGraph(text,function,input.error))return std::nullopt;
                const auto key="file:"+PathKey(physical);
                input.dependencies.sources[key]=applied==sourceOverrides.end()?MaterialCompiler::MaterialDependencySource{key,physical,text,
                    Hash(ReadText(NLS::Core::Assets::GetAssetMetaPath(physical))),0,{}}:applied->second;
                return function;
            };
            const auto generated=MaterialCompiler::GenerateMaterialGraphShader(graph,functions);
            if(!generated.Succeeded()){input.error=generated.diagnostics[0].message;return input;}
            shader=generated.shader;schema=generated.schema;
        }
        else
        {
            const auto parsed=Render::ShaderLab::ParseShaderLabSource(shaderSourceText,shaderPath.generic_string());
            if(!parsed.Succeeded()){input.error=parsed.DiagnosticsToString();return input;}
            shader=parsed.asset;schema=MaterialCompiler::BuildMaterialParameterSchema(shader,shaderOwner);
        }
        const auto canonicalize=[&](auto& material)
        {
            auto overrides=std::move(material.parameters);material.parameters.clear();
            for(const auto& [name,value]:overrides)
            {
                const auto* definition=value.id?schema.Find(value.id):schema.Find(name);
                if(!definition || definition->type!=value.type){material.orphanParameters.push_back(value);continue;}
                auto current=value;current.id=definition->id;current.name=definition->name;material.parameters[current.name]=std::move(current);
            }
        };
        canonicalize(input.resolved);for(auto& dependent:input.dependents)canonicalize(dependent.resolved);
        Render::ShaderLab::ShaderLabKeywordSet keywords;
        for(const auto& parameter:schema.parameters)if(parameter.isStatic && std::holds_alternative<bool>(parameter.defaultValue))
            input.resolved.staticParameters.try_emplace(parameter.name,std::get<bool>(parameter.defaultValue));
        for (const auto& [name,value] : input.resolved.staticParameters) if (value) keywords.Enable(name);
        std::vector<Render::ShaderLab::ShaderLabKeywordSet> variants{keywords};
        for (const auto& dependent : input.dependents)
        {
            Render::ShaderLab::ShaderLabKeywordSet variant;
            std::map<std::string,bool> settings;
            for(const auto& parameter:schema.parameters)if(parameter.isStatic && std::holds_alternative<bool>(parameter.defaultValue))
                settings[parameter.name]=std::get<bool>(parameter.defaultValue);
            for(const auto& [name,value]:dependent.resolved.staticParameters)settings[name]=value;
            for (const auto& [name,value] : settings) if(value)variant.Enable(name);
            variants.push_back(std::move(variant));
        }
        const auto base=std::make_shared<const MaterialCompiler::MaterialDependencySnapshot>(input.dependencies);
        for (const auto& sub : shader.subShaders)
            for (const auto& pass : sub.passes)
            {
                if (!pass.tags.values.contains("LightMode")) continue;
                MaterialCompiler::MaterialShaderCompileOptions options;
                options.sourcePath=shaderPath;options.sourceIdentity=shaderIdentity;
                options.subAssetKey="shader:"+shader.shaderName+"/"+pass.name;
                options.generatedPath=cache/"Generated"/(std::to_string(Render::Resources::MakeMaterialParameterId(shaderIdentity,pass.name))+".hlsl");
                options.cacheDatabasePath=(cache/"ShaderCache.tsv").string();
                options.includeDirectories={engineShaders.string(),shaderPath.parent_path().parent_path().string()};
                options.targets={target};options.variants=variants;options.generateGlsl=false;options.dependencies=base;
                options.variantBudget=variantBudget;
                options.sourceOverrides=sourceOverrides;
                std::shared_ptr<const MaterialCompiler::MaterialDependencySnapshot> captured;
                options.capturedDependencies=&captured;
                auto artifact=MaterialCompiler::CompileMaterialPass(pass,options,schema);
                for (const auto& stage : artifact.stages)
                    if (stage.output.status!=Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
                    {input.error=stage.output.diagnostics;return input;}
                if (captured)
                    for (const auto& [key,source] : captured->sources)
                    {
                        const auto existing=input.dependencies.sources.find(key);
                        if (existing!=input.dependencies.sources.end() && existing->second.content!=source.content)
                        {input.error="Material dependency changed between pass compilations";return input;}
                        input.dependencies.sources[key]=source;
                    }
                input.dependencies.context+=';'+artifact.dependencySnapshotId;
                input.passes.push_back(std::move(artifact));
            }
        if (input.passes.empty()) input.error="Shader has no runtime material Pass";
        return input;
    });
    session.error.clear();return true;
}

bool MaterialEditingService::Current(const CompiledMaterialEdit& result,const MaterialEditSession& session) const
{
    if (result.documentSource!=session.document.Source()) return false;
    for (const auto& [key,source] : result.dependencies.sources)
    {
        const bool working=session.workingDependencies.contains(key) || (!source.path.empty() &&
            (session.workingDependencies.contains("file:"+PathKey(source.path)) || session.workingDependencies.contains(PathKey(source.path))));
        if (key=="material:"+session.document.Asset().identity.Key()) continue;
        if (!source.path.empty() && source.path.generic_string().find("MaterialEditor/Generated")!=std::string::npos) continue;
        if (key.starts_with("material:"))
        {
            std::string error;
            const auto parsed=Render::Assets::DeserializeMaterialAsset(source.content,error);
            if (!parsed) return false;
            const auto current=ReadAsset(parsed->identity,session.workingDependencies.contains(key),error);
            if (!current || Render::Assets::SerializeMaterialAsset(*current)!=source.content) return false;
        }
        else if (const auto applied=m_appliedSources.find(key);
            applied!=m_appliedSources.end() && !working)
        {if (applied->second.content!=source.content || applied->second.metadataHash!=source.metadataHash ||
            (applied->second.appliedGeneration!=source.appliedGeneration &&
                !(session.document.AppliedSnapshot()==result.dependencies.Id() && applied->second.appliedGeneration==session.appliedGeneration))) return false;}
        else if (!source.path.empty() && source.path.extension()!=".mat" &&
            source.path.generic_string().find("MaterialEditor/Generated")==std::string::npos)
        {if (ReadSource(source.path,working)!=source.content || (!source.metadataHash.empty() &&
            Hash(ReadText(NLS::Core::Assets::GetAssetMetaPath(source.path)))!=source.metadataHash)) return false;}
    }
    return true;
}

std::unique_ptr<Render::Resources::Material> MaterialEditingService::BuildRuntime(const CompiledMaterialEdit& result,std::string& error)
{
    Render::Resources::Loaders::MaterialLoader::LoadOptions options;
    options.loadMissingTextures=NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>();
    for (const auto& pass : result.passes)
    {
        const auto bytes=Render::Assets::SerializeShaderArtifact(pass);
        auto hash=NLS::Core::Assets::BuildArtifactStorageFileName(bytes.data(),bytes.size());
        const auto path=m_projectRoot/"Library"/"Artifacts"/"MaterialEditor"/hash;
        std::filesystem::create_directories(path.parent_path());
        if (!std::filesystem::exists(path))
        {std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            if (!file) {error="Cannot stage material runtime shader";return {};}}
        NLS::Core::Assets::RegisterRuntimeAuthorizedArtifactPath(NLS::Core::Assets::TryMakePortableContentArtifactPath(path.generic_string()));
        options.shaderPassArtifactPaths.push_back(path.string());
    }
    auto resolved=result.resolved;
    for(auto& [name,parameter]:resolved.parameters)
        if(auto* path=std::get_if<std::string>(&parameter.value);path && result.textures.contains(*path))
        {
            const auto frozen=result.dependencies.sources.find("texture:"+*path);
            if(frozen!=result.dependencies.sources.end())*path=frozen->second.path.generic_string();
        }
    if(!result.passes.empty())
    {
        const auto& schema=result.passes.front().parameterSchema;
        auto overrides=std::move(resolved.parameters);resolved.parameters.clear();
        for(const auto& [name,override]:overrides)
        {
            const auto* parameter=override.id?schema.Find(override.id):schema.Find(name);
            if(!parameter || parameter->type!=override.type){resolved.orphanParameters.push_back(override);continue;}
            auto current=override;current.name=parameter->name;current.id=parameter->id;resolved.parameters[current.name]=std::move(current);
        }
    }
    auto material=std::unique_ptr<Render::Resources::Material>(Render::Resources::Loaders::MaterialLoader::CreateFromSerializedPayload(
        "MaterialEditorPreview",Render::Assets::BuildResolvedMaterialPayload(resolved),options));
    if (!material || !material->IsValid()) {error="Compiled material could not create a runtime resource";return {};}
    material->SetAssetData(std::make_shared<const Render::Assets::MaterialAsset>(result.asset));
    return material;
}

void MaterialEditingService::Pump()
{
    bool boundary=false;
    auto drain=[&] {if(!boundary) {if(auto* driver=Render::Context::TryGetLocatedDriver())
        Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);boundary=true;}};
    for (auto& [key,session] : m_sessions)
    {
        if(session->pendingCook.valid() && session->pendingCook.wait_for(std::chrono::seconds(0))==std::future_status::ready)
        {
            try{auto result=session->pendingCook.get();session->error=result.error;
                if(result.Succeeded())session->lastRelease=result.databasePath.parent_path().parent_path();}
            catch(const std::exception& ex){session->error=ex.what();}
        }
        if (!session->previewUpdates.empty() && session->preview)
        {
            drain();
            for (const auto& [name,value] : session->previewUpdates)
            {std::string error;
                const auto* path=std::get_if<std::string>(&value);
                // Texture changes require a frozen artifact dependency capture.
                if(!path)Render::Resources::ApplyMaterialParameter(*session->preview,name,value,error);}
            session->previewUpdates.clear();
        }
        if (session->applyRequested && !session->pending.valid())
        {session->applyRequested=false;session->preparePublication=true;if(!RequestPreview(*session))session->preparePublication=false;}
        if (!session->pending.valid() || session->pending.wait_for(std::chrono::seconds(0))!=std::future_status::ready) continue;
        CompiledMaterialEdit result;
        try{result=session->pending.get();}
        catch(const std::exception& exception){session->error=exception.what();session->failedSource=session->requestedSource;
            session->preparePublication=false;session->applyRequested=false;continue;}
        if (!Current(result,*session))
        {session->failedSource.clear();if(result.publication){session->preparePublication=false;session->applyRequested=false;
            session->error="Publication cancelled because its dependency snapshot changed";}continue;}
        if (!result.error.empty()) {session->error=result.error;session->failedSource=result.documentSource;
            session->preparePublication=false;session->applyRequested=false;continue;}
        auto preview=BuildRuntime(result,session->error);
        if (!preview) {session->failedSource=result.documentSource;session->preparePublication=false;session->applyRequested=false;continue;}
        if (session->preview) {drain();session->preview->CopyRuntimeFrom(*preview);}
        else session->preview=std::move(preview);
        if (session->draft) session->draft->CopyRuntimeFrom(*session->preview);
        else session->draft=session->preview->CreateDynamicCopy();
        session->compiled=std::move(result);session->failedSource.clear();session->error.clear();
        if (session->compiled->publication) {session->preparePublication=false;Publish(*session);}
    }
}

bool MaterialEditingService::Save(MaterialEditSession& session)
{
    if (!session.document.Save()) {session.error=session.document.Error();return false;}
    session.error.clear();return true;
}

bool MaterialEditingService::ApplyToScene(MaterialEditSession& session)
{
    session.preparePublication=true;session.failedSource.clear();
    if (session.pending.valid()) {session.applyRequested=true;return true;}
    const auto started=RequestPreview(session);
    if(!started){session.preparePublication=false;session.applyRequested=false;}
    return started;
}

bool MaterialEditingService::PublishResources(MaterialEditSession& session)
{
    if(session.pendingCook.valid())return true;
    if(!session.compiled || !session.document.IsApplied() || !Current(*session.compiled,session))
    {session.error="Apply the material and selected dependency snapshot before publishing resources";return false;}
    const auto compiled=*session.compiled;
    if(compiled.passes.empty()){session.error="Material has no compiled passes";return false;}
    MaterialCompiler::MaterialCookOptions options;
    options.compiler.sourceIdentity=compiled.resolved.shader.path;
    options.compiler.sourcePath=ResolvePath(compiled.resolved.shader.path);
    options.compiler.generatedPath=m_projectRoot/"Library"/"MaterialCook"/"Generated"/"Material.hlsl";
    options.compiler.cacheDatabasePath=(m_projectRoot/"Library"/"MaterialCook"/"ShaderCache.tsv").string();
    options.compiler.includeDirectories={(m_engineRoot/"Shaders").string()};
    options.compiler.targets={Render::ShaderCompiler::ShaderTargetPlatform::DXIL,Render::ShaderCompiler::ShaderTargetPlatform::SPIRV};
    options.compiler.variantBudget=session.variantBudget;options.compiler.generateGlsl=false;
    options.compiler.dependencies=std::make_shared<const MaterialCompiler::MaterialDependencySnapshot>(compiled.dependencies);
    options.compiler.sourceOverrides=compiled.dependencies.sources;
    options.frozenTextures=compiled.textures;
    options.shaderId=NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic(compiled.resolved.shader.path));
    if(!compiled.resolved.shader.guid.empty())options.shaderId=NLS::Core::Assets::AssetId(NLS::Guid::Parse(compiled.resolved.shader.guid));
    auto release=compiled.dependencies.Id();release=release.substr(release.find(':')+1);
    options.outputRoot=m_projectRoot/"Library"/"MaterialReleases"/(release+"-"+std::to_string(m_publication));
    const auto database=Assets::AssetDatabaseFacade::CreateReadOnlySnapshot(*m_database);
    options.textureProvider=[database](std::string_view path)->std::optional<MaterialCompiler::MaterialCookTexture>
    {
        for(const auto& entry:database->LoadAllAssetsAtPath(std::string(path)))
        {if(entry.artifactType!=NLS::Core::Assets::ArtifactType::Texture)continue;
            const auto physical=database->ResolveArtifactPathAtPath(std::string(path),entry.subAssetKey);
            std::ifstream file(physical,std::ios::binary);if(!file)return std::nullopt;
            return MaterialCompiler::MaterialCookTexture{entry.assetId,entry.subAssetKey,
                std::vector<uint8_t>{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()}};}
        return std::nullopt;
    };
    std::vector<MaterialCompiler::MaterialCookEntry> materials;
    const auto id=NLS::Core::Assets::AssetId(NLS::Guid::Parse(compiled.asset.identity.guid));
    materials.push_back({id,compiled.asset.identity.path,compiled.resolved});
    for(const auto& child:compiled.dependents)materials.push_back({NLS::Core::Assets::AssetId(NLS::Guid::Parse(child.asset.identity.guid)),child.asset.identity.path,child.resolved});
    session.pendingCook=std::async(std::launch::async,[options=std::move(options),materials=std::move(materials),compiled]() mutable
    {
        Render::ShaderLab::ShaderLabAssetDesc shader;
        const auto source=std::find_if(compiled.dependencies.sources.begin(),compiled.dependencies.sources.end(),[&](const auto& item)
            {return !item.second.path.empty() && PathKey(item.second.path)==PathKey(options.compiler.sourcePath);});
        if(source==compiled.dependencies.sources.end())return MaterialCompiler::MaterialCookResult{{},{},{},"Publication has no frozen shader source"};
        if(options.compiler.sourcePath.extension()==".shadergraph")
        {
            MaterialCompiler::MaterialGraphAsset graph;std::string error;
            if(!MaterialCompiler::DeserializeMaterialGraph(source->second.content,graph,error))return MaterialCompiler::MaterialCookResult{{},{},{},error};
            const auto generated=MaterialCompiler::GenerateMaterialGraphShader(graph,[&](const auto& reference)->std::optional<MaterialCompiler::MaterialGraphAsset>
            {
                for(const auto& [key,item]:compiled.dependencies.sources)
                    if(item.path.extension()==".shaderfunc" && (item.path.generic_string().ends_with(reference.path) || item.identity==reference.Key()))
                    {MaterialCompiler::MaterialGraphAsset function;std::string e;if(MaterialCompiler::DeserializeMaterialGraph(item.content,function,e))return function;}
                return std::nullopt;
            });
            if(!generated.Succeeded())return MaterialCompiler::MaterialCookResult{{},{},{},generated.diagnostics.front().message};shader=generated.shader;
        }
        else
        {const auto parsed=Render::ShaderLab::ParseShaderLabSource(source->second.content,options.compiler.sourcePath.generic_string());
            if(!parsed.Succeeded())return MaterialCompiler::MaterialCookResult{{},{},{},parsed.DiagnosticsToString()};shader=parsed.asset;}
        return MaterialCompiler::CookMaterialFamily(shader,compiled.passes.front().parameterSchema,materials,options);
    });
    session.error.clear();return true;
}

bool MaterialEditingService::Publish(MaterialEditSession& session)
{
    if (!session.compiled || !session.preview || !Current(*session.compiled,session))
    {session.error="Wait for the current dependency snapshot to compile before applying";return false;}
    std::vector<std::pair<Render::Resources::Material*,std::unique_ptr<Render::Resources::Material>>> candidates;
    if (session.sceneMaterial && Render::Resources::Material::IsLive(session.sceneMaterial) && session.sceneMaterial->GetInstanceId()==session.sceneMaterialId)
        candidates.emplace_back(session.sceneMaterial,session.preview->CreateDynamicCopy());
    if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
        for (const auto& [key,material] : NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResources())
            if (material && !material->IsDynamicInstance() && material->GetAssetData() &&
                material->GetAssetData()->identity.Key()==session.document.Asset().identity.Key() &&
                std::none_of(candidates.begin(),candidates.end(),[material](const auto& item){return item.first==material;}))
                candidates.emplace_back(material,session.preview->CreateDynamicCopy());
    for (const auto& dependent : session.compiled->dependents)
    {
        if (!Render::Resources::Material::IsLive(dependent.target) || dependent.target->GetInstanceId()!=dependent.instanceId)
        {session.error="A dependent material was unloaded before publication";return false;}
        auto result=*session.compiled;
        result.asset=dependent.asset;result.resolved=dependent.resolved;
        auto runtime=BuildRuntime(result,session.error);if(!runtime)return false;
        candidates.emplace_back(dependent.target,std::move(runtime));
    }
    if (auto* driver=Render::Context::TryGetLocatedDriver()) Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);
    if (!Current(*session.compiled,session)) {session.error="Dependencies changed before publication";return false;}
    auto archiveId=session.compiled->dependencies.Id();archiveId=archiveId.substr(archiveId.find(':')+1);
    if(!session.compiled->dependencies.Save(m_projectRoot/"Library"/"MaterialEditor"/"Publications"/(archiveId+".json"),session.error))return false;
    for (auto& [destination,candidate] : candidates)
    {destination->CopyRuntimeFrom(*candidate);Render::Resources::MaterialInstance::NotifyBaseChanged(*destination);}
    ++m_publication;
    session.appliedGeneration=m_publication;
    for (const auto& [key,source] : session.compiled->dependencies.sources)
    {
        if (!source.path.empty() && source.path.generic_string().find("MaterialEditor/Generated")!=std::string::npos) continue;
        auto applied=source;
        const auto existing=m_appliedSources.find(key);
        applied.appliedGeneration=existing!=m_appliedSources.end() && existing->second.content==source.content &&
            existing->second.metadataHash==source.metadataHash ? existing->second.appliedGeneration : m_publication;
        m_appliedSources[key]=applied;
        if (!source.path.empty() && source.path.extension()!=".mat")
        {auto file=applied;file.identity="file:"+PathKey(source.path);
            m_appliedSources[file.identity]=std::move(file);}
    }
    session.document.MarkApplied(session.compiled->dependencies.Id(),session.document.Source());
    for(auto& [key,program]:m_sourceSessions)
        if(const auto found=session.compiled->dependencies.sources.find("file:"+key);found!=session.compiled->dependencies.sources.end())
        {program->appliedSource=found->second.content;program->appliedSnapshot=session.compiled->dependencies.Id();}
    for (const auto& dependent : session.compiled->dependents)
        for (const auto& [key,child] : m_sessions)
            if (child->document.Asset().identity.Key()==dependent.asset.identity.Key())
                child->document.MarkApplied(session.compiled->dependencies.Id(),Render::Assets::SerializeMaterialAsset(dependent.asset));
    session.error.clear();return true;
}

bool MaterialEditingService::SetParameter(MaterialEditSession& session,const Render::Resources::MaterialParameterDesc& parameter,
    Render::Resources::MaterialParameterValue value)
{
    if(!Render::Resources::ValidateMaterialParameterValue(parameter.type,value))
    {session.error="Parameter value has an incompatible type or is not finite";return false;}
    auto candidate=session.document.Asset();
    if(parameter.isStatic)
    {
        const auto* enabled=std::get_if<bool>(&value);
        if(!enabled){session.error="Static switches require a boolean value";return false;}
        candidate.staticParameters[parameter.name]=*enabled;
        return session.document.Apply(std::move(candidate),"Set "+parameter.displayName);
    }
    auto found=std::find_if(candidate.parameters.begin(),candidate.parameters.end(),[&](const auto& item)
        {return item.id==parameter.id || item.name==parameter.name;});
    Render::Assets::MaterialParameterOverride override{parameter.id,parameter.name,parameter.type,std::move(value)};
    if (found==candidate.parameters.end()) candidate.parameters.push_back(std::move(override));else *found=std::move(override);
    const auto result=session.document.Apply(std::move(candidate),"Edit "+parameter.displayName);
    if (result && session.preview)
    {std::string error;const auto& latest=session.document.Asset().parameters;
        const auto found=std::find_if(latest.begin(),latest.end(),[&](const auto& item){return item.id==parameter.id || item.name==parameter.name;});
        if (found!=latest.end()) session.previewUpdates[parameter.name]=found->value;}
    if (!result) session.error=session.document.Error();
    return result;
}
bool MaterialEditingService::ClearParameter(MaterialEditSession& session,Render::Resources::MaterialParameterId id,std::string_view name)
{
    auto candidate=session.document.Asset();
    std::erase_if(candidate.parameters,[&](const auto& item){return item.id==id || item.name==name;});
    candidate.staticParameters.erase(std::string(name));
    return session.document.Apply(std::move(candidate),"Inherit material parameter");
}

bool MaterialEditingService::CaptureInspectorChanges(MaterialEditSession& session,const Render::Resources::Material& before)
{
    if (!session.draft) return false;
    const auto& after=*session.draft;
    using Value=Render::Resources::MaterialParameterValue;
    const auto encode=[](const Render::Resources::Material& material,const std::string& name,const std::any& value)->std::optional<Value>
    {
        if(value.type()==typeid(float))return std::any_cast<float>(value);
        if(value.type()==typeid(int))return static_cast<int32_t>(std::any_cast<int>(value));
        if(value.type()==typeid(bool))return std::any_cast<bool>(value);
        if(value.type()==typeid(Maths::Vector2)){auto v=std::any_cast<Maths::Vector2>(value);return std::array<float,4>{v.x,v.y,0,0};}
        if(value.type()==typeid(Maths::Vector3)){auto v=std::any_cast<Maths::Vector3>(value);return std::array<float,4>{v.x,v.y,v.z,0};}
        if(value.type()==typeid(Maths::Vector4)){auto v=std::any_cast<Maths::Vector4>(value);return std::array<float,4>{v.x,v.y,v.z,v.w};}
        if(value.type()==typeid(Maths::Matrix4)){auto m=std::any_cast<Maths::Matrix4>(value);std::array<float,16> v{};std::copy(m.data,m.data+16,v.begin());return v;}
        if(value.type()==typeid(Render::Resources::Texture2D*))
        {const auto* texture=std::any_cast<Render::Resources::Texture2D*>(value);return texture?texture->path:std::string{};}
        if(value.type()==typeid(std::string))return std::any_cast<std::string>(value);
        return std::nullopt;
    };
    auto candidate=session.document.Asset();
    if (before.GetShader()!=after.GetShader())
    {
        if (!candidate.parent.Empty()) {session.error="Change the parent material to change an inherited shader";return false;}
        candidate.shader.path=after.GetShader()?after.GetShader()->GetImportedArtifactSourcePath():std::string{};
        candidate.shader.guid.clear();
    }
    if(before.IsBlendable()!=after.IsBlendable()) candidate.renderStateOverrides["surfaceMode"]=Render::Resources::MaterialSurfaceModeName(after.GetSurfaceMode());
    if(before.HasBackfaceCulling()!=after.HasBackfaceCulling() || before.HasFrontfaceCulling()!=after.HasFrontfaceCulling())
        candidate.renderStateOverrides["doubleSided"]=!after.HasBackfaceCulling() && !after.HasFrontfaceCulling()?"true":"false";
    if(before.HasDepthWriting()!=after.HasDepthWriting()) candidate.renderStateOverrides["depthWrite"]=after.HasDepthWriting()?"true":"false";
    for (const auto& [name,raw] : after.GetUniformsData())
    {
        const auto value=encode(after,name,raw);
        const auto* oldRaw=before.GetParameterBlock().TryGet(name);
        const auto old=oldRaw?encode(before,name,*oldRaw):std::nullopt;
        if (!value || value==old) continue;
        const auto schema=after.GetShader()?after.GetShader()->GetMaterialParameterSchema():nullptr;
        const auto* descriptor=schema?schema->Find(name):nullptr;
        const auto info=after.GetShader()?after.GetShader()->GetUniformInfo(name):std::nullopt;
        if (!info) continue;
        const auto found=std::find_if(candidate.parameters.begin(),candidate.parameters.end(),[&](const auto& item){return item.name==name;});
        Render::Assets::MaterialParameterOverride override{descriptor?descriptor->id:0,name,info->type,*value};
        if(found==candidate.parameters.end())candidate.parameters.push_back(override);else *found=override;
        session.previewUpdates[name]=*value;
    }
    if (!session.document.SetWorkingFromExternal(std::move(candidate))) {session.error=session.document.Error();return false;}
    session.failedSource.clear();return true;
}
}
