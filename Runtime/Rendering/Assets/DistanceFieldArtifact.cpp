#include "Rendering/Assets/DistanceFieldArtifact.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Assets/ArtifactFileReader.h"
#include "Rendering/Assets/SceneImportPipeline.h"

namespace NLS::Render::DistanceField
{
    namespace
    {
        using Bytes = std::vector<uint8_t>;
        constexpr auto Type = NLS::Core::Assets::ArtifactType::MeshDistanceField;
        void Word(Bytes& b,uint32_t w) { for(int i=0;i<4;++i) b.push_back(uint8_t(w>>(8*i))); }
        void Float(Bytes& b,float f) { Word(b,std::bit_cast<uint32_t>(f)); }
        void Text(Bytes& b,const std::string& text) { Word(b,static_cast<uint32_t>(text.size())); b.insert(b.end(),text.begin(),text.end()); }
        void Mip(Bytes& b,const DistanceFieldMip& mip)
        {
            for(auto n:mip.grid) Word(b,n);
            Float(b,mip.maxDistance); Word(b,static_cast<uint32_t>(mip.indirection.size()));
            Word(b,static_cast<uint32_t>(mip.bricks.size()));
            for(auto entry:mip.indirection) Word(b,entry);
            b.insert(b.end(),mip.bricks.begin(),mip.bricks.end());
        }
        struct Reader
        {
            const uint8_t* data; size_t size,offset=0; bool ok=true;
            uint32_t Word()
            {
                if(offset>size || size-offset<4) { ok=false; return 0; }
                uint32_t w=0; for(int i=0;i<4;++i) w|=uint32_t(data[offset++])<<(8*i);
                return w;
            }
            float Float() { return std::bit_cast<float>(Word()); }
            std::string Text()
            {
                const auto n=Word();
                if(!ok || n>size-offset || n>1024*1024) { ok=false; return {}; }
                std::string text(reinterpret_cast<const char*>(data+offset),n); offset+=n; return text;
            }
            DistanceFieldMip Mip()
            {
                DistanceFieldMip mip;
                for(auto& n:mip.grid) n=Word();
                mip.maxDistance=Float(); const uint32_t entries=Word(),bytes=Word();
                if(!ok || uint64_t(entries)*4+bytes>size-offset || entries>128u*128u*128u || bytes%512)
                { ok=false; return mip; }
                mip.indirection.reserve(entries);
                for(uint32_t i=0;i<entries;++i) mip.indirection.push_back(Word());
                mip.bricks.assign(data+offset,data+offset+bytes); offset+=bytes;
                ok=ok && ValidateDistanceFieldMip(mip); return mip;
            }
        };
        Bytes Wrap(Bytes payload)
        {
            NLS::Core::Assets::NativeArtifactMetadata metadata;
            metadata.artifactType=Type; metadata.schemaName="mesh-distance-field"; metadata.schemaVersion=1;
            return NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata),payload);
        }
        std::mutex sourceMutex;
        std::unordered_map<std::string,MeshDistanceFieldSource> sources;
        std::unordered_map<std::string,std::vector<std::string>> sourceOwners;
        std::atomic_uint64_t sourceRevision{1};
        std::string Normalize(const std::filesystem::path& path) { return path.lexically_normal().generic_string(); }
        std::string Path(const std::filesystem::path& root,const std::string& path)
        {
            const auto value=std::filesystem::path(path);
            if(value.is_absolute() || root.empty()) return Normalize(value);
            const auto portable=value.generic_string();
            if(root.filename()=="Artifacts" && portable.starts_with("Library/Artifacts/") && root.parent_path().filename()=="Library")
                return Normalize(root.parent_path().parent_path()/value);
            if(root.filename()=="Artifacts" && portable.starts_with("Artifacts/"))
                return Normalize(root.parent_path()/value);
            return Normalize(root/value);
        }
    }

    std::string DistanceFieldSubAssetKey(const std::string& key,uint32_t mip)
    { return "distancefield:"+key+(mip<MipCount?":mip"+std::to_string(mip):""); }

    std::vector<uint8_t> SerializeDistanceFieldArtifact(const MeshDistanceFieldArtifact& asset)
    {
        if(!ValidateDistanceFieldMip(asset.mips[2])) return {};
        Bytes b; Word(b,0x3146444e); Word(b,asset.twoSided);
        for(int a=0;a<3;++a) Float(b,asset.boundsMin.array[a]);
        for(int a=0;a<3;++a) Float(b,asset.boundsMax.array[a]);
        Text(b,asset.buildIdentity);
        for(uint32_t i=0;i<2;++i)
        {
            for(auto n:asset.mips[i].grid) Word(b,n);
            Float(b,asset.mips[i].maxDistance); Text(b,asset.mipPaths[i]);
        }
        Mip(b,asset.mips[2]); return Wrap(std::move(b));
    }
    std::vector<uint8_t> SerializeDistanceFieldMip(const DistanceFieldMip& mip)
    {
        if(!ValidateDistanceFieldMip(mip)) return {};
        Bytes b; Word(b,0x314d444e); Mip(b,mip); return Wrap(std::move(b));
    }
    std::optional<MeshDistanceFieldArtifact> DeserializeDistanceFieldArtifact(const Bytes& bytes)
    {
        auto view=NLS::Core::Assets::ReadNativeArtifactContainerView(bytes,Type,1);
        if(!view) return {};
        Reader r{view->payloadData,view->payloadSize};
        if(r.Word()!=0x3146444e) return {};
        MeshDistanceFieldArtifact a; a.twoSided=r.Word()!=0;
        for(int i=0;i<3;++i) a.boundsMin.array[i]=r.Float();
        for(int i=0;i<3;++i) { a.boundsMax.array[i]=r.Float(); if(!std::isfinite(a.boundsMax.array[i]) || !std::isfinite(a.boundsMin.array[i]) || a.boundsMax.array[i]<=a.boundsMin.array[i]) return {}; }
        a.buildIdentity=r.Text();
        for(uint32_t i=0;i<2;++i)
        {
            for(auto& n:a.mips[i].grid) { n=r.Word(); if(!n || n>128) return {}; }
            a.mips[i].maxDistance=r.Float();
            if(!std::isfinite(a.mips[i].maxDistance) || a.mips[i].maxDistance<=0) return {};
            a.mipPaths[i]=r.Text();
        }
        a.mips[2]=r.Mip();
        if(!r.ok || r.offset!=r.size || a.buildIdentity.empty()) return {};
        return a;
    }
    std::optional<DistanceFieldMip> DeserializeDistanceFieldMip(const Bytes& bytes)
    {
        auto view=NLS::Core::Assets::ReadNativeArtifactContainerView(bytes,Type,1);
        if(!view) return {};
        Reader r{view->payloadData,view->payloadSize};
        if(r.Word()!=0x314d444e) return {};
        auto mip=r.Mip(); if(!r.ok || r.offset!=r.size) return {}; return mip;
    }
    std::shared_ptr<MeshDistanceFieldArtifact> LoadDistanceFieldArtifact(const std::filesystem::path& path)
    {
        const auto portable=NLS::Core::Assets::TryMakePortableContentArtifactPath(path.generic_string());
        if(!portable.empty() && !NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portable)) return {};
        Bytes bytes; if(!Assets::Detail::ReadArtifactFileBytes(path,bytes,nullptr)) return {};
        auto asset=DeserializeDistanceFieldArtifact(bytes);
        return asset?std::make_shared<MeshDistanceFieldArtifact>(std::move(*asset)):nullptr;
    }
    std::optional<DistanceFieldMip> LoadDistanceFieldMip(const std::filesystem::path& path)
    {
        const auto portable=NLS::Core::Assets::TryMakePortableContentArtifactPath(path.generic_string());
        if(!portable.empty() && !NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portable)) return {};
        Bytes bytes; if(!Assets::Detail::ReadArtifactFileBytes(path,bytes,nullptr)) return {};
        return DeserializeDistanceFieldMip(bytes);
    }
    void RegisterDistanceFieldManifest(const NLS::Core::Assets::ArtifactManifest& manifest,const std::filesystem::path& root)
    {
        std::lock_guard lock(sourceMutex);
        bool changed=false;
        const auto owner=manifest.sourceAssetId.ToString()+"/"+manifest.targetPlatform+"/"+Normalize(root);
        std::vector<std::string> currentKeys;
        std::unordered_map<std::string,std::vector<std::string>> groupMembers;
        for(const auto& mesh:manifest.subAssets)
            if(mesh.artifactType==NLS::Core::Assets::ArtifactType::Mesh && mesh.subAssetKey.starts_with("mesh:") &&
                manifest.FindSubAsset(DistanceFieldSubAssetKey(mesh.subAssetKey)))
                if(auto primitive=Assets::ParsePrimitiveMeshSourceKey(mesh.subAssetKey.substr(5)))
                    groupMembers[primitive->first].push_back(Path(root,mesh.artifactPath));
        for(const auto& mesh:manifest.subAssets)
        {
            if(mesh.artifactType!=NLS::Core::Assets::ArtifactType::Mesh) continue;
            const auto key=Path(root,mesh.artifactPath);
            const auto* main=manifest.FindSubAsset(DistanceFieldSubAssetKey(mesh.subAssetKey));
            if(!main) { changed=sources.erase(key)!=0 || changed; continue; }
            currentKeys.push_back(key);
            MeshDistanceFieldSource source;
            source.mainPath=Path(root,main->artifactPath); source.contentHash=main->contentHash;
            for(uint32_t mip=0;mip<2;++mip)
                if(const auto* entry=manifest.FindSubAsset(DistanceFieldSubAssetKey(mesh.subAssetKey,mip))) source.mipPaths[mip]=Path(root,entry->artifactPath);
            if(mesh.subAssetKey.starts_with("mesh:"))
                if(auto primitive=Assets::ParsePrimitiveMeshSourceKey(mesh.subAssetKey.substr(5)))
                {
                    const auto groupKey=DistanceFieldSubAssetKey("mesh:"+primitive->first);
                    if(const auto* group=manifest.FindSubAsset(groupKey))
                    {
                        source.groupMainPath=Path(root,group->artifactPath);
                        source.groupMembers=groupMembers[primitive->first];
                        for(uint32_t mip=0;mip<2;++mip)
                            if(const auto* entry=manifest.FindSubAsset(groupKey+":mip"+std::to_string(mip))) source.groupMipPaths[mip]=Path(root,entry->artifactPath);
                        MeshDistanceFieldSource joint;
                        joint.mainPath=source.groupMainPath; joint.mipPaths=source.groupMipPaths; joint.contentHash=group->contentHash;
                        const auto existing=sources.find(joint.mainPath);
                        if(existing==sources.end() || existing->second!=joint) { sources[joint.mainPath]=joint; changed=true; }
                        currentKeys.push_back(joint.mainPath);
                    }
                }
            const auto found=sources.find(key);
            if(found==sources.end() || found->second!=source) { sources[key]=std::move(source); changed=true; }
        }
        if(const auto old=sourceOwners.find(owner);old!=sourceOwners.end())
            for(const auto& key:old->second)
                if(std::find(currentKeys.begin(),currentKeys.end(),key)==currentKeys.end()) changed=sources.erase(key)!=0 || changed;
        sourceOwners[owner]=std::move(currentKeys);
        if(changed) ++sourceRevision;
    }
    std::optional<MeshDistanceFieldSource> FindMeshDistanceFieldSource(const std::string& path)
    {
        std::lock_guard lock(sourceMutex); auto found=sources.find(Normalize(path));
        // Player manifests retain portable content paths; MeshManager may have
        // resolved the same mesh against the install root before registering it.
        if(found==sources.end())
        {
            const auto portable=NLS::Core::Assets::TryMakePortableContentArtifactPath(path);
            if(!portable.empty()) found=sources.find(portable);
        }
        return found==sources.end()?std::nullopt:std::optional(found->second);
    }
    uint64_t GetDistanceFieldSourceRevision() { return sourceRevision.load(); }
    bool HasDistanceFieldSources() { std::lock_guard lock(sourceMutex); return !sources.empty(); }
}
