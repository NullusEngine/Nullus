#include "MaterialInstance.h"
#include "Shader.h"
#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Math/Matrix4.h"
#include <mutex>
#include <vector>
#include <algorithm>
#include <set>

namespace NLS::Render::Resources
{
void Material::SetExternalBuffer(std::string name,std::shared_ptr<RHI::RHIBuffer> buffer)
{
    auto found=m_externalBuffers.find(name);
    if(found!=m_externalBuffers.end()&&found->second==buffer)return;
    if(buffer)m_externalBuffers[std::move(name)]=std::move(buffer);else m_externalBuffers.erase(name);
    RebuildBindingLayout();TouchRenderStateRevision();
}
void Material::SetOwnedCubeTexture(const std::string& name,std::shared_ptr<TextureCube> texture)
{
    auto* pointer=texture.get();
    if(texture)m_ownedCubeTextures[name]=std::move(texture);else m_ownedCubeTextures.erase(name);
    SetRawParameter(name,pointer);
}
void Material::SetOwnedTexture2D(const std::string& name,std::shared_ptr<Texture2D> texture)
{
    auto* pointer=texture.get();
    if(texture)m_ownedTextures2D[name]=std::move(texture);else m_ownedTextures2D.erase(name);
    SetRawParameter(name,pointer);
}

bool ApplyMaterialCubeTexture(Material& material,std::string_view name,std::string_view path,std::string& error)
{
    const std::string value(path);
    if(value.empty())
    {material.SetOwnedCubeTexture(std::string(name),{});material.ClearTextureResourcePath(std::string(name));error.clear();return true;}
    auto physical=std::filesystem::path(value);
    if(!physical.is_absolute())physical=NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(value);
    std::optional<Assets::TextureArtifactData> artifact;
    if(value=="white" || value=="black" || value=="bump")
    {
        artifact.emplace();artifact->width=artifact->height=1;artifact->dimension=RHI::TextureDimension::TextureCube;artifact->arrayLayers=6;
        for(uint32_t face=0;face<6;++face)
        {Assets::TextureArtifactMip mip;mip.width=mip.height=1;mip.rowPitch=mip.slicePitch=4;
            mip.pixels=value=="black"?std::vector<uint8_t>{0,0,0,255}:value=="bump"?std::vector<uint8_t>{128,128,255,255}:std::vector<uint8_t>{255,255,255,255};
            artifact->mips.push_back(std::move(mip));Assets::TextureArtifactSubresource sub;sub.arrayLayer=face;sub.width=sub.height=1;sub.rowPitch=sub.slicePitch=4;
            artifact->subresources.push_back(sub);}
    }
    else artifact=Assets::LoadTextureArtifact(physical);
    if(!artifact || artifact->dimension!=RHI::TextureDimension::TextureCube || artifact->arrayLayers!=6 || artifact->subresources.size()!=artifact->mips.size())
    {error="Expected a native cubemap artifact: "+value;return false;}
    auto cube=std::make_shared<TextureCube>();
    if(auto* driver=Context::TryGetLocatedDriver())
        if(const auto device=Context::DriverRendererAccess::GetExplicitDevice(*driver))
        {
            RHI::RHITextureDesc desc;desc.dimension=RHI::TextureDimension::TextureCube;desc.arrayLayers=6;
            desc.extent={artifact->width,artifact->height,1};desc.format=artifact->format;
            desc.colorSpace=artifact->colorSpace==Assets::TextureArtifactColorSpace::Srgb?RHI::TextureColorSpace::SRGB:RHI::TextureColorSpace::Linear;
            desc.mipLevels=0;for(const auto& item:artifact->subresources)desc.mipLevels=std::max(desc.mipLevels,item.level+1);
            desc.usage=RHI::TextureUsageFlags::Sampled;desc.debugName="MaterialCube:"+std::string(name);
            RHI::RHITextureUploadDesc upload;upload.subresources.resize(6*desc.mipLevels);upload.extent=desc.extent;
            for(size_t i=0;i<artifact->mips.size();++i)
            {const auto& mip=artifact->mips[i];const auto& sub=artifact->subresources[i];
                upload.subresources[sub.arrayLayer*desc.mipLevels+sub.level]={mip.PixelData(),mip.PixelSize()};upload.dataSize+=mip.PixelSize();}
            const auto resource=device->CreateTexture(desc,upload);
            if(!resource){error="Cannot upload material cubemap: "+value;return false;}
            cube->SetRHITexture(resource);
        }
    material.SetOwnedCubeTexture(std::string(name),std::move(cube));
    material.SetTextureResourcePath(std::string(name),value);
    error.clear();return true;
}
namespace
{
std::mutex& InstanceMutex(){static std::mutex mutex;return mutex;}
std::vector<MaterialInstance*>& Instances(){static std::vector<MaterialInstance*> instances;return instances;}
}
MaterialInstance::MaterialInstance(const Material& base)
    : m_base(base.CreateDynamicCopy()), m_material(base.CreateDynamicCopy()),m_baseId(base.GetInstanceId())
{std::lock_guard lock(InstanceMutex());Instances().push_back(this);}

MaterialInstance::~MaterialInstance()
{std::lock_guard lock(InstanceMutex());std::erase(Instances(),this);}

void MaterialInstance::NotifyBaseChanged(const Material& base)
{
    std::lock_guard lock(InstanceMutex());
    std::vector<const Material*> pending{&base};std::set<uint64_t> visited;
    while(!pending.empty())
    {const auto* changed=pending.back();pending.pop_back();if(!visited.insert(changed->GetInstanceId()).second)continue;
        for(auto* instance:Instances())if(instance->m_baseId==changed->GetInstanceId())
        {std::string error;instance->Rebase(*changed,error);pending.push_back(instance->m_material.get());}}
}

bool ApplyMaterialParameter(Material& material,std::string_view name, const MaterialParameterValue& value, std::string& error)
{
    const auto* shader = material.GetShader();
    const auto schema = shader ? shader->GetMaterialParameterSchema() : nullptr;
    const auto* descriptor = schema ? schema->Find(name) : nullptr;
    if (descriptor && descriptor->isStatic)
    { error = "Static parameters require a compiled variant: " + std::string(name); return false; }
    const auto info = shader ? shader->GetUniformInfo(std::string(name)) : std::nullopt;
    if (!info) { error = "Unknown material parameter: " + std::string(name); return false; }
    if(!ValidateMaterialParameterValue(info->type,value))
    {error="Material parameter has an incompatible type or nonfinite value: "+std::string(name);return false;}
    if(info->type==UniformType::UNIFORM_SAMPLER_2D || info->type==UniformType::UNIFORM_SAMPLER_CUBE)
    {
        const auto& path=std::get<std::string>(value);
        if(info->type==UniformType::UNIFORM_SAMPLER_CUBE)return ApplyMaterialCubeTexture(material,name,path,error);
        if(path=="white" || path=="black" || path=="bump")
        {
            const uint8_t r=path=="white"?255:path=="black"?0:128;
            auto* texture=Loaders::TextureLoader::CreatePixel(r,r,path=="black"?0:255,255);
            if(!texture){error="Cannot create default material texture";return false;}
            texture->path=path;
            material.SetOwnedTexture2D(std::string(name),std::shared_ptr<Texture2D>(texture));material.SetTextureResourcePath(std::string(name),path);error.clear();return true;
        }
        Texture2D* texture=nullptr;
        if(!path.empty() && path!="white" && path!="black" && path!="bump")
        {
            if(!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
            {error="Texture manager is unavailable";return false;}
            texture=NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>().GetResource(path);
            if(!texture){error="Material texture cannot be resolved: "+path;return false;}
        }
        material.SetRawParameter(std::string(name),texture);
        material.SetTextureResourcePath(std::string(name),path);error.clear();return true;
    }
    std::any encoded;
    if (info->type == UniformType::UNIFORM_FLOAT && std::holds_alternative<float>(value)) encoded = std::get<float>(value);
    else if (info->type == UniformType::UNIFORM_INT && std::holds_alternative<int32_t>(value)) encoded = static_cast<int>(std::get<int32_t>(value));
    else if (info->type == UniformType::UNIFORM_BOOL && std::holds_alternative<bool>(value)) encoded = std::get<bool>(value);
    else if (info->type == UniformType::UNIFORM_FLOAT_MAT4 && std::holds_alternative<std::array<float,16>>(value))
    { Maths::Matrix4 matrix; const auto& values=std::get<std::array<float,16>>(value);
        for(size_t index=0;index<values.size();++index) matrix.data[index]=values[index]; encoded=matrix; }
    else if (const auto* vector = std::get_if<std::array<float,4>>(&value))
    {
        if (info->type == UniformType::UNIFORM_FLOAT_VEC2) encoded = Maths::Vector2((*vector)[0],(*vector)[1]);
        else if (info->type == UniformType::UNIFORM_FLOAT_VEC3) encoded = Maths::Vector3((*vector)[0],(*vector)[1],(*vector)[2]);
        else if (info->type == UniformType::UNIFORM_FLOAT_VEC4) encoded = Maths::Vector4((*vector)[0],(*vector)[1],(*vector)[2],(*vector)[3]);
    }
    if (!encoded.has_value()) { error = "Material parameter value has an incompatible type: " + std::string(name); return false; }
    material.SetRawParameter(std::string(name),std::move(encoded));
    error.clear(); return true;
}

bool MaterialInstance::Apply(std::string_view name,const MaterialParameterValue& value,std::string& error)
{ return ApplyMaterialParameter(*m_material,name,value,error); }

bool MaterialInstance::SetParameter(std::string name, MaterialParameterValue value, std::string& error)
{
    if (!Apply(name,value,error)) return false;
    m_overrides[std::move(name)] = std::move(value);
    return true;
}

bool MaterialInstance::ClearParameter(std::string_view name, std::string& error)
{
    const auto it = m_overrides.find(std::string(name));
    if (it == m_overrides.end()) { error.clear(); return true; }
    m_overrides.erase(it);
    m_material->CopyRuntimeFrom(*m_base);
    std::string firstError;
    for(const auto& [key,value]:m_overrides)
        if(!Apply(key,value,error) && firstError.empty())firstError=error;
    error=firstError;return error.empty();
}

bool MaterialInstance::Rebase(const Material& base, std::string& error)
{
    if(base.GetInstanceId()==m_material->GetInstanceId()){error="A dynamic material cannot inherit itself";return false;}
    m_baseId=base.GetInstanceId();
    m_base->CopyRuntimeFrom(base);
    m_material->CopyRuntimeFrom(base);
    std::string firstError;
    for (const auto& [name,value] : m_overrides)
        if (!Apply(name,value,error) && firstError.empty()) firstError = error;
    error = firstError;
    return error.empty();
}
}
