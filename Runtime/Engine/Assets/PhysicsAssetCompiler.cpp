#include "PhysicsAssetCompiler.h"
#include <Json/json.hpp>
#include <Platform/Process/Process.h>
#include <Serialize/ObjectGraphDocument.h>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace NLS::Engine::Assets
{
namespace
{
using Json=nlohmann::json;
Json Parse(std::string_view text)
{
    if (text.empty() || text.size()>64u*1024u*1024u) throw std::runtime_error("Physics source is empty or exceeds 64 MiB.");
    std::vector<std::set<std::string>> keys;
    auto result=Json::parse(text.begin(),text.end(),[&](int depth,Json::parse_event_t event,Json& value)
    {
        if(depth>32) throw std::runtime_error("Physics source nesting exceeds 32 levels.");
        if (event==Json::parse_event_t::object_start) keys.emplace_back();
        else if (event==Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::runtime_error("Duplicate physics source field: "+value.get<std::string>());
        else if (event==Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
    if (!result.is_object() || !result.contains("version") || !result["version"].is_number_integer() || result["version"]!=1)
        throw std::runtime_error("Physics source requires object schema version 1.");
    return result;
}
void Fields(const Json& object,std::initializer_list<std::string_view> allowed)
{
    if (!object.is_object()) throw std::runtime_error("Physics source must be an object.");
    for(auto it=object.begin();it!=object.end();++it)
        if(std::find(allowed.begin(),allowed.end(),it.key())==allowed.end()) throw std::runtime_error("Unknown physics source field: "+it.key());
}
float Number(const Json& value)
{
    if (!value.is_number()) throw std::runtime_error("Physics numeric field has the wrong type.");
    const auto number=value.get<double>();
    if (!std::isfinite(number) || std::abs(number)>std::numeric_limits<float>::max()) throw std::runtime_error("Physics number is not finite or representable.");
    return static_cast<float>(number);
}
float Number(const Json& root,const char* key,float fallback,float low,float high)
{
    const auto value=root.contains(key)?Number(root[key]):fallback;
    if(value<low || value>high) throw std::runtime_error(std::string("Physics field is out of range: ")+key);
    return value;
}
uint32_t Integer(const Json& value,uint32_t maximum)
{
    if(!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>()<0))
        throw std::runtime_error("Physics integer field must be nonnegative.");
    const auto result=value.get<uint64_t>(); if(result>maximum) throw std::runtime_error("Physics integer field exceeds its limit.");
    return static_cast<uint32_t>(result);
}
Maths::Vector3 Vector(const Json& value)
{
    if(!value.is_array() || value.size()!=3) throw std::runtime_error("Physics vector requires exactly three numbers.");
    return {Number(value[0]),Number(value[1]),Number(value[2])};
}
Physics::MaterialCombineMode Combine(const Json& root,const char* key)
{
    if(!root.contains(key)) return Physics::MaterialCombineMode::Average;
    if(!root[key].is_string()) throw std::runtime_error("Physics combine mode must be a string.");
    const auto value=root[key].get<std::string>();
    if(value=="average") return Physics::MaterialCombineMode::Average;
    if(value=="minimum") return Physics::MaterialCombineMode::Minimum;
    if(value=="multiply") return Physics::MaterialCombineMode::Multiply;
    if(value=="maximum") return Physics::MaterialCombineMode::Maximum;
    throw std::runtime_error("Unknown physics combine mode.");
}
const char* CombineName(Physics::MaterialCombineMode mode)
{
    switch(mode)
    {
    case Physics::MaterialCombineMode::Minimum: return "minimum";
    case Physics::MaterialCombineMode::Multiply: return "multiply";
    case Physics::MaterialCombineMode::Maximum: return "maximum";
    default: return "average";
    }
}
PhysicsMaterialData Material(const Json& root)
{
    Fields(root,{"version","staticFriction","dynamicFriction","restitution","friction2D","restitution2D","frictionCombine","restitutionCombine"});
    PhysicsMaterialData data;
    data.staticFriction=Number(root,"staticFriction",0.5f,0,10000);
    data.dynamicFriction=Number(root,"dynamicFriction",0.5f,0,10000);
    data.restitution=Number(root,"restitution",0.5f,0,1);
    data.friction2D=Number(root,"friction2D",0.5f,0,10000); data.restitution2D=Number(root,"restitution2D",0,0,1);
    data.frictionCombine=Combine(root,"frictionCombine"); data.restitutionCombine=Combine(root,"restitutionCombine"); return data;
}
}
std::string PhysicsCookingFingerprint() { return "physics-assets2:"+Physics::CookedCollisionMesh::CompatibilityTag(); }
namespace
{
Serialize::ObjectIdentifier Reference(const Json& value,const char* subAssetKey)
{
    Fields(value,{"guid","fileID","type"});
    const auto guid=Guid::Parse(value.at("guid").get<std::string>());
    if(!guid.IsValid()) throw std::runtime_error("Shape resource reference requires a nonzero GUID.");
    auto result=Serialize::ObjectIdentifier::Asset(Serialize::AssetId(guid),Serialize::MakeLocalIdentifierInFile(guid,subAssetKey));
    if(value.contains("fileID"))
    {
        const auto& id=value["fileID"];
        if(!id.is_number_integer() || (id.is_number_unsigned() && id.get<uint64_t>()>uint64_t(INT64_MAX)) ||
            id.get<int64_t>()!=result.localIdentifierInFile)
            throw std::runtime_error("Shape reference fileID does not match the resource's stable main subasset.");
    }
    if(value.contains("type")) result.fileType=static_cast<Serialize::FileType>(Integer(value["type"],3));
    if(!result.IsAsset()) throw std::runtime_error("Shape resources require an asset file type.");
    return result;
}
std::shared_ptr<const PhysicsShapeData> Shape(const Json& root)
{
    Fields(root,{"version","shapes"}); const auto& parts=root.at("shapes");
    if(!parts.is_array() || parts.empty() || parts.size()>Physics::MaxCompoundShapes3D)
        throw std::runtime_error("Shape resource requires between 1 and 256 parts.");
    auto result=std::make_shared<PhysicsShapeData>(); result->parts.reserve(parts.size()); std::set<uint32_t> ids;
    for(const auto& value:parts)
    {
        PhysicsShapePart part; part.id=Integer(value.at("id"),UINT32_MAX);
        if(!part.id || !ids.insert(part.id).second) throw std::runtime_error("Shape part IDs must be positive and unique.");
        const auto type=value.at("type").get<std::string>();
        if(type=="box")
        {
            Fields(value,{"id","type","position","rotation","scale","material","halfExtents"});
            if(value.contains("halfExtents")) part.geometry.halfExtents=Vector(value["halfExtents"]);
            const auto v=part.geometry.halfExtents;
            if(v.x<1e-6f || v.y<1e-6f || v.z<1e-6f || v.x>1e7f || v.y>1e7f || v.z>1e7f)
                throw std::runtime_error("Shape box half extents are out of range.");
        }
        else if(type=="sphere" || type=="capsule" || type=="cylinder")
        {
            if(type=="sphere") Fields(value,{"id","type","position","rotation","scale","material","radius"});
            else Fields(value,{"id","type","position","rotation","scale","material","radius","halfHeight"});
            part.geometry.type=type=="sphere"?Physics::CollisionGeometryType3D::Sphere:
                type=="capsule"?Physics::CollisionGeometryType3D::Capsule:Physics::CollisionGeometryType3D::Cylinder;
            part.geometry.radius=Number(value,"radius",0.5f,1e-6f,1e7f);
            part.geometry.halfHeight=Number(value,"halfHeight",0.5f,type=="cylinder"?1e-6f:0,1e7f);
        }
        else if(type=="mesh")
        {
            Fields(value,{"id","type","position","rotation","scale","material","mesh"});
            part.geometry.type=Physics::CollisionGeometryType3D::CookedMesh;
            part.collisionMesh=Reference(value.at("mesh"),"collision-mesh:main");
        }
        else throw std::runtime_error("Unknown shape resource geometry type.");
        if(value.contains("position")) part.position=Vector(value["position"]);
        if(value.contains("scale")) part.scale=Vector(value["scale"]);
        for(const auto v:{part.position.x,part.position.y,part.position.z})
            if(std::abs(v)>1e7f) throw std::runtime_error("Shape part position is out of range.");
        for(const auto v:{part.scale.x,part.scale.y,part.scale.z})
            if(std::abs(v)<1e-6f || std::abs(v)>1e7f) throw std::runtime_error("Shape part scale is out of range.");
        if(value.contains("rotation"))
        {
            const auto& q=value["rotation"];
            if(!q.is_array() || q.size()!=4) throw std::runtime_error("Shape rotation requires a quaternion with four numbers.");
            double length=0; float values[4];
            for(size_t i=0;i<4;++i) { values[i]=Number(q[i]); length+=double(values[i])*values[i]; }
            if(length<1e-12) throw std::runtime_error("Shape rotation quaternion is degenerate.");
            const double divisor=std::sqrt(length)*(values[3]<0?-1:1);
            part.rotation={float(values[0]/divisor),float(values[1]/divisor),float(values[2]/divisor),float(values[3]/divisor)};
        }
        if(value.contains("material")) part.material=Reference(value["material"],"physics-material:main");
        result->parts.push_back(std::move(part));
    }
    std::sort(result->parts.begin(),result->parts.end(),[](const auto& a,const auto& b){return a.id<b.id;}); return result;
}
Json CanonicalReference(const Serialize::ObjectIdentifier& ref)
{ return {{"guid",ref.guid.ToString()},{"fileID",ref.localIdentifierInFile},{"type",static_cast<int32_t>(ref.fileType)}}; }
Json CanonicalShape(const PhysicsShapeData& data,Core::Assets::ArtifactPayload& artifact)
{
    Json parts=Json::array(); std::set<std::pair<std::string,std::string>> dependencies;
    for(const auto& part:data.parts)
    {
        const auto& geometry=part.geometry;
        Json value={{"id",part.id},{"position",{part.position.x,part.position.y,part.position.z}},
            {"scale",{part.scale.x,part.scale.y,part.scale.z}},{"rotation",{part.rotation.x,part.rotation.y,part.rotation.z,part.rotation.w}}};
        if(geometry.type==Physics::CollisionGeometryType3D::Box)
        { value["type"]="box"; value["halfExtents"]={geometry.halfExtents.x,geometry.halfExtents.y,geometry.halfExtents.z}; }
        else if(geometry.type==Physics::CollisionGeometryType3D::CookedMesh)
        {
            value["type"]="mesh"; value["mesh"]=CanonicalReference(part.collisionMesh);
            dependencies.emplace(part.collisionMesh.guid.ToString(),"collision-mesh:main");
        }
        else
        {
            value["type"]=geometry.type==Physics::CollisionGeometryType3D::Sphere?"sphere":
                geometry.type==Physics::CollisionGeometryType3D::Capsule?"capsule":"cylinder";
            value["radius"]=geometry.radius;
            if(geometry.type!=Physics::CollisionGeometryType3D::Sphere) value["halfHeight"]=geometry.halfHeight;
        }
        if(part.material.IsAsset())
        { value["material"]=CanonicalReference(part.material); dependencies.emplace(part.material.guid.ToString(),"physics-material:main"); }
        parts.push_back(std::move(value));
    }
    for(const auto& [guid,key]:dependencies) artifact.dependencies.push_back({Core::Assets::AssetDependencyKind::ImportedArtifact,guid,key});
    return {{"version",1},{"shapes",std::move(parts)}};
}
}
std::shared_ptr<const PhysicsShapeData> ParsePhysicsShape(std::string_view source,std::string& error)
{
    try { auto result=Shape(Parse(source)); error.clear(); return result; }
    catch(const std::exception& exception) { error=exception.what(); return {}; }
}
bool CapturePhysicsAssetSource(const std::filesystem::path& path,std::vector<uint8_t>& bytes,std::string& error)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    const auto count=file?file.tellg():std::streampos(-1);
    if(count<=0 || count>std::streampos(64u*1024u*1024u)) { error="Physics source is unavailable, empty or exceeds 64 MiB."; return false; }
    std::vector<uint8_t> snapshot(static_cast<size_t>(count)); file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(snapshot.data()),static_cast<std::streamsize>(snapshot.size())))
    { error="Physics source changed or could not be read completely."; return false; }
    bytes=std::move(snapshot); error.clear(); return true;
}
bool IsPhysicsCookingTargetSupported(std::string_view target,std::string_view architecture)
{
    const auto host=Platform::Process::CurrentArtifactPlatformTag();
    const auto tag=Physics::CookedCollisionMesh::CompatibilityTag();
    const bool platform=target=="editor" || target==host || (target.size()>host.size() && target.substr(0,host.size())==host && target[host.size()]=='-');
    return platform && (architecture.empty() || (architecture=="x64" && tag.find("-x64-")!=std::string::npos) ||
        (architecture=="arm64" && tag.find("-arm64-")!=std::string::npos));
}
bool ParsePhysicsMaterial(std::string_view source,PhysicsMaterialData& data,std::string& error)
{
    try { auto parsed=Material(Parse(source)); data=parsed; error.clear(); return true; }
    catch(const std::exception& exception) { error=exception.what(); return false; }
}
bool CompilePhysicsAsset(Core::Assets::AssetType type,std::string_view source,std::string_view targetPlatform,
    Core::Assets::ArtifactPayload& artifact,std::string& error)
{
    try
    {
        const auto root=Parse(source); Core::Assets::ArtifactPayload result;
        if (targetPlatform.empty() || targetPlatform.size()>128) throw std::runtime_error("Physics asset requires a target platform.");
        if(type==Core::Assets::AssetType::PhysicsMaterial)
        {
            const auto data=Material(root);
            const Json canonical={{"version",1},{"staticFriction",data.staticFriction},{"dynamicFriction",data.dynamicFriction},
                {"restitution",data.restitution},{"friction2D",data.friction2D},{"restitution2D",data.restitution2D},
                {"frictionCombine",CombineName(data.frictionCombine)},{"restitutionCombine",CombineName(data.restitutionCombine)}};
            const auto text=canonical.dump(); result.payload.assign(text.begin(),text.end());
            result.artifactType=Core::Assets::ArtifactType::PhysicsMaterial; result.loaderId="physics-material"; result.subAssetKey="physics-material:main";
        }
        else if(type==Core::Assets::AssetType::PhysicsShape)
        {
            const auto data=Shape(root); const auto text=CanonicalShape(*data,result).dump();
            result.payload.assign(text.begin(),text.end()); result.artifactType=Core::Assets::ArtifactType::PhysicsShape;
            result.loaderId="physics-shape"; result.subAssetKey="physics-shape:main";
        }
        else if(type==Core::Assets::AssetType::CollisionMesh)
        {
            if(!IsPhysicsCookingTargetSupported(targetPlatform)) throw std::runtime_error("Collision cooking requires a matching native host; cook this target on its own platform.");
            Fields(root,{"version","kind","vertices","indices","heights","rows","columns","rowSpacing","columnSpacing","bakeScale","weldVertices","weldTolerance","convexVertexLimit"});
            Physics::CollisionMeshSource input;
            const auto kind=root.at("kind").get<std::string>();
            if(kind=="convex") input.kind=Physics::CollisionMeshKind::Convex;
            else if(kind=="triangle") input.kind=Physics::CollisionMeshKind::Triangle;
            else if(kind=="heightfield") input.kind=Physics::CollisionMeshKind::HeightField;
            else throw std::runtime_error("Unknown collision mesh kind.");
            if(root.contains("bakeScale")) input.bakeScale=Vector(root["bakeScale"]);
            if(root.contains("weldVertices"))
            {
                if(!root["weldVertices"].is_boolean()) throw std::runtime_error("weldVertices requires a boolean.");
                input.weldVertices=root["weldVertices"].get<bool>();
            }
            input.weldTolerance=Number(root,"weldTolerance",0.001f,0,1000);
            if(root.contains("convexVertexLimit")) input.convexVertexLimit=Integer(root["convexVertexLimit"],255);
            if(root.contains("vertices"))
            {
                const auto& values=root["vertices"]; if(!values.is_array() || values.size()>(1u<<20)) throw std::runtime_error("Invalid collision vertex array.");
                input.vertices.reserve(values.size()); for(const auto& value:values) input.vertices.push_back(Vector(value));
            }
            if(root.contains("indices"))
            {
                const auto& values=root["indices"]; if(!values.is_array() || values.size()>3u*(1u<<20)) throw std::runtime_error("Invalid collision index array.");
                input.indices.reserve(values.size()); for(const auto& value:values) input.indices.push_back(Integer(value,UINT32_MAX));
            }
            if(root.contains("heights"))
            {
                const auto& values=root["heights"]; if(!values.is_array() || values.size()>(1u<<20)) throw std::runtime_error("Invalid height array.");
                input.heights.reserve(values.size()); for(const auto& value:values) input.heights.push_back(Number(value));
            }
            if(root.contains("rows")) input.rows=Integer(root["rows"],1u<<20);
            if(root.contains("columns")) input.columns=Integer(root["columns"],1u<<20);
            input.rowSpacing=Number(root,"rowSpacing",1,1e-4f,1e7f); input.columnSpacing=Number(root,"columnSpacing",1,1e-4f,1e7f);
            auto cooked=Physics::CookedCollisionMesh::Cook(input,error); if(!cooked) return false;
            result.payload=cooked->Serialize(); result.artifactType=Core::Assets::ArtifactType::CollisionMesh;
            result.loaderId="collision-mesh"; result.subAssetKey="collision-mesh:main";
        }
        else throw std::runtime_error("Unsupported physics asset type.");
        result.relativePath="physics"; result.dependencies.push_back({Core::Assets::AssetDependencyKind::PostprocessorVersion,"nullus-physics-cooking",PhysicsCookingFingerprint()});
        artifact=std::move(result); error.clear(); return true;
    }
    catch(const std::exception& exception) { error=exception.what(); return false; }
}
}
