#include "PhysicsMaterial.h"
#include "CollisionMeshAsset.h"
#include "PhysicsAssetCompiler.h"
#include "PhysicsAssetReadScope.h"
#include "RuntimeAssetDatabase.h"
#include <Serialize/ObjectGraphDocument.h>
#include "Engine/Serialize/ObjectGraphDocument.h"
#include <Assets/ArtifactDatabase.h>
#include <Assets/NativeArtifactContainer.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ServiceLocator.h>
#include <Platform/Process/Process.h>
#include <Rendering/Assets/ArtifactFileReader.h>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace NLS::Engine::Assets
{
namespace
{
using namespace Core::Assets;
struct LookupSnapshot
{
    std::filesystem::path root;
    ArtifactDatabase database;
    bool attempted=false,readable=false;
};
thread_local uint32_t lookupDepth=0;
thread_local std::unique_ptr<LookupSnapshot> lookupSnapshot;
std::filesystem::path ProjectRoot()
{
    auto assets=std::filesystem::path(Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());
    if(assets.filename().empty()) assets=assets.parent_path();
    return assets.parent_path();
}
std::string PhysicsSchema(ArtifactType type)
{
    switch(type)
    {
    case ArtifactType::CollisionMesh: return "collision-mesh";
    case ArtifactType::PhysicsShape: return "physics-shape";
    default: return "physics-material";
    }
}
std::optional<NativeArtifactContainer> ReadPhysicsArtifact(const std::filesystem::path& path,const std::string& hash,
    ArtifactType type,std::string& error,const Serialize::ObjectIdentifier* expected)
{
    Render::Assets::Detail::ArtifactFileReader file(path);
    if(!file.IsOpen() || file.Size()>uint64_t(65)*1024*1024) { error="Physics artifact is unavailable or exceeds 65 MiB."; return {}; }
    std::vector<uint8_t> bytes(static_cast<size_t>(file.Size()));
    const Render::Assets::Detail::ArtifactFileReadRange range{bytes.data(),bytes.size()};
    if(!file.Read(0,std::span(&range,1),nullptr)) { error="Cannot read physics artifact."; return {}; }
    const auto actual="sha256:"+ToHexString(ComputeSha256(bytes.data(),bytes.size()));
    if(hash!=actual) { error="Physics artifact content does not match its manifest hash."; return {}; }
    auto result=ReadNativeArtifactContainer(bytes,type,1);
    const auto schema=PhysicsSchema(type);
    if(!result || result->metadata.schemaName!=schema || result->metadata.subAssetKey!=schema+":main" || !result->metadata.sourceAssetId.IsValid())
    { error="Physics artifact type, schema or source identity is invalid."; return {}; }
    if(Core::ServiceLocator::Contains<RuntimeAssetDatabase>() && !IsRuntimeAssetPlatformCompatible(type,result->metadata.targetPlatform,
        Core::ServiceLocator::Get<RuntimeAssetDatabase>().GetManifest().targetPlatform))
    { error="Physics artifact target does not match the active runtime manifest."; return {}; }
    if(expected && (expected->guid!=result->metadata.sourceAssetId.GetGuid() || expected->localIdentifierInFile!=
        Serialize::MakeLocalIdentifierInFile(expected->guid,result->metadata.subAssetKey)))
    { error="Physics artifact identity does not match the requested stable reference."; return {}; }
    return result;
}
std::optional<RuntimeAssetManifestEntry> Resolve(const Serialize::ObjectIdentifier& identifier,ArtifactType type,std::string& error)
{
    if(!identifier.IsAsset()) { error="Physics resources require a stable asset reference."; return {}; }
    std::optional<RuntimeAssetManifestEntry> result;
    if(Core::ServiceLocator::Contains<RuntimeAssetDatabase>())
    {
        const auto* entry=Core::ServiceLocator::Get<RuntimeAssetDatabase>().ResolveByLocalIdentifierInFile(AssetId(identifier.guid),identifier.localIdentifierInFile);
        if(entry) result=*entry;
    }
    else
    {
        std::unique_ptr<LookupSnapshot> local;
        LookupSnapshot* snapshot;
        if(lookupDepth)
        {
            if(!lookupSnapshot || lookupSnapshot->root!=ProjectRoot()) lookupSnapshot=std::make_unique<LookupSnapshot>();
            snapshot=lookupSnapshot.get();
        }
        else { local=std::make_unique<LookupSnapshot>(); snapshot=local.get(); }
        if(!snapshot->attempted)
        {
            snapshot->root=ProjectRoot(); snapshot->attempted=true;
            snapshot->readable=snapshot->database.Load(snapshot->root/"Library/ArtifactDB");
        }
        if(snapshot->readable)
        {
            auto manifest=snapshot->database.BuildManifestForSource(AssetId(identifier.guid),"editor");
            if(!manifest) manifest=snapshot->database.BuildManifestForSource(AssetId(identifier.guid),Platform::Process::CurrentArtifactPlatformTag());
            if(manifest) for(const auto& entry:manifest->subAssets)
                if(Serialize::MakeLocalIdentifierInFile(identifier.guid,entry.subAssetKey)==identifier.localIdentifierInFile)
                    result=RuntimeAssetManifestEntry{entry.sourceAssetId,entry.subAssetKey,entry.artifactType,entry.loaderId,entry.artifactPath,entry.contentHash,{}};
        }
    }
    const auto loader=PhysicsSchema(type);
    if(!result || result->artifactType!=type || result->loaderId!=loader || !IsRuntimePackagedAssetPath(result->artifactPath))
    { error="Physics resource is missing or invalid in the active manifest/ArtifactDB."; return {}; }
    return result;
}
template<class T>
std::shared_ptr<T> Load(const Serialize::ObjectIdentifier& identifier,ArtifactType type,std::string& error)
{
    const auto entry=Resolve(identifier,type,error); if(!entry) return {};
    auto path=std::filesystem::path(entry->artifactPath); if(path.is_relative()) path=ProjectRoot()/path;
    struct CacheEntry { std::weak_ptr<T> asset; std::string context; };
    static std::mutex mutex; static std::unordered_map<std::string,CacheEntry> cache;
    const auto key=ProjectRoot().generic_string()+":"+identifier.guid.ToString()+":"+std::to_string(identifier.localIdentifierInFile)+":"+
        std::to_string(static_cast<int32_t>(identifier.fileType));
    std::lock_guard lock(mutex); std::erase_if(cache,[](const auto& item){return item.second.asset.expired();});
    auto& cached=cache[key]; auto asset=cached.asset.lock(); if(!asset) asset=std::make_shared<T>();
    const auto context=Core::ServiceLocator::Contains<RuntimeAssetDatabase>()?
        Core::ServiceLocator::Get<RuntimeAssetDatabase>().GetManifest().targetPlatform:std::string("authoring");
    if((asset->ContentHash()!=entry->contentHash || cached.context!=context) && !asset->Reload(path,entry->contentHash,error,&identifier)) return {};
    if(Serialize::PersistentManager::Instance().BindObjectIdentifier(*asset,identifier)==NLS::InstanceID_None)
    { error="Physics reference is already bound to another object."; return {}; }
    cached={asset,context}; error.clear(); return asset;
}
}
PhysicsAssetReadScope::PhysicsAssetReadScope() { ++lookupDepth; }
PhysicsAssetReadScope::~PhysicsAssetReadScope() { if(--lookupDepth==0) lookupSnapshot.reset(); }
bool PhysicsMaterial::Reload(const std::filesystem::path& path,std::string hash,std::string& error,const Serialize::ObjectIdentifier* expected)
{
    Serialize::ObjectIdentifier bound;
    if(!expected && Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(GetInstanceID(),bound) && bound.IsAsset()) expected=&bound;
    const auto container=ReadPhysicsArtifact(path,hash,Core::Assets::ArtifactType::PhysicsMaterial,error,expected); if(!container) return false;
    PhysicsMaterialData data;
    if(!ParsePhysicsMaterial(std::string_view(reinterpret_cast<const char*>(container->payload.data()),container->payload.size()),data,error)) return false;
    m_data=std::make_shared<const PhysicsMaterialData>(data); m_hash=std::move(hash); error.clear(); return true;
}
bool CollisionMeshAsset::Reload(const std::filesystem::path& path,std::string hash,std::string& error,const Serialize::ObjectIdentifier* expected)
{
    Serialize::ObjectIdentifier bound;
    if(!expected && Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(GetInstanceID(),bound) && bound.IsAsset()) expected=&bound;
    const auto container=ReadPhysicsArtifact(path,hash,Core::Assets::ArtifactType::CollisionMesh,error,expected); if(!container) return false;
    auto mesh=Physics::CookedCollisionMesh::Deserialize(container->payload,error); if(!mesh) return false;
    m_mesh=std::move(mesh); m_hash=std::move(hash); error.clear(); return true;
}
std::shared_ptr<PhysicsMaterial> LoadPhysicsMaterial(const Serialize::ObjectIdentifier& identifier,std::string& error)
{ return Load<PhysicsMaterial>(identifier,Core::Assets::ArtifactType::PhysicsMaterial,error); }
std::shared_ptr<CollisionMeshAsset> LoadCollisionMeshAsset(const Serialize::ObjectIdentifier& identifier,std::string& error)
{ return Load<CollisionMeshAsset>(identifier,Core::Assets::ArtifactType::CollisionMesh,error); }
bool PhysicsShapeAsset::Reload(const std::filesystem::path& path,std::string hash,std::string& error,const Serialize::ObjectIdentifier* expected)
{
    Serialize::ObjectIdentifier bound;
    if(!expected && Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(GetInstanceID(),bound) && bound.IsAsset()) expected=&bound;
    const auto container=ReadPhysicsArtifact(path,hash,Core::Assets::ArtifactType::PhysicsShape,error,expected); if(!container) return false;
    auto data=ParsePhysicsShape(std::string_view(reinterpret_cast<const char*>(container->payload.data()),container->payload.size()),error);
    if(!data) return false;
    m_data=std::move(data); m_hash=std::move(hash); error.clear(); return true;
}
std::shared_ptr<PhysicsShapeAsset> LoadPhysicsShapeAsset(const Serialize::ObjectIdentifier& identifier,std::string& error)
{ return Load<PhysicsShapeAsset>(identifier,Core::Assets::ArtifactType::PhysicsShape,error); }
}
