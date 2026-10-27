#pragma once
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "Rendering/Assets/MaterialAsset.h"
#include "Rendering/ShaderLab/ShaderLabTypes.h"

namespace NLS::MaterialCompiler
{
enum class MaterialGraphType : uint8_t {Float,Float2,Float3,Float4,Int,Bool,Texture2D,TextureCube,Sampler};
enum class MaterialGraphDomain : uint8_t {Surface,ParticleSprite};
enum class MaterialGraphShading : uint8_t {PBR,Unlit};
enum class MaterialGraphBlend : uint8_t {Opaque,Masked,Alpha,Premultiplied,Additive};

struct MaterialGraphLink
{
    uint64_t node = 0;
    uint32_t output = 0;
    bool operator==(const MaterialGraphLink&) const = default;
};
struct MaterialGraphPort
{
    uint64_t id = 0;
    std::string name;
    MaterialGraphType type = MaterialGraphType::Float;
};
struct MaterialGraphParameter
{
    Render::Resources::MaterialParameterId id = 0;
    std::string name;
    std::string group;
    MaterialGraphType type = MaterialGraphType::Float;
    Render::Resources::MaterialParameterValue value = 0.f;
    bool color = false;
    bool isStatic = false;
    std::optional<std::array<float,2>> range;
};
struct MaterialGraphNode
{
    uint64_t id = 0;
    std::string operation = "Constant";
    MaterialGraphType type = MaterialGraphType::Float;
    std::string name;
    std::array<float,4> value{};
    uint64_t parameter = 0;
    std::map<std::string,MaterialGraphLink> inputs;
    std::vector<MaterialGraphPort> customInputs;
    std::vector<MaterialGraphPort> customOutputs;
    std::string code;
    std::vector<std::string> includes;
    Render::Assets::MaterialAssetReference function;
    std::array<float,2> position{};
};
struct MaterialGraphAsset
{
    uint32_t version = 1;
    std::string id;
    std::string name = "Material Graph";
    bool function = false;
    MaterialGraphDomain domain = MaterialGraphDomain::Surface;
    MaterialGraphShading shading = MaterialGraphShading::PBR;
    MaterialGraphBlend blend = MaterialGraphBlend::Opaque;
    bool doubleSided = false;
    float maximumWorldDisplacement = 0;
    std::vector<MaterialGraphParameter> parameters;
    std::vector<MaterialGraphPort> functionInputs;
    std::vector<MaterialGraphPort> functionOutputs;
    std::vector<MaterialGraphNode> nodes;
    std::map<std::string,MaterialGraphLink> outputs;
    std::map<uint64_t,std::string> comments;
};
struct MaterialGraphDiagnostic
{
    std::string graph;
    uint64_t node = 0;
    std::string pin;
    std::string message;
};
struct MaterialGraphSource
{
    Render::ShaderLab::ShaderLabAssetDesc shader;
    Render::Resources::MaterialParameterSchema schema;
    std::vector<MaterialGraphDiagnostic> diagnostics;
    std::vector<Render::Assets::MaterialAssetReference> dependencies;
    bool Succeeded() const {return diagnostics.empty();}
};

using MaterialGraphFunctionProvider=std::function<std::optional<MaterialGraphAsset>(const Render::Assets::MaterialAssetReference&)>;
std::string SerializeMaterialGraph(const MaterialGraphAsset& asset);
bool DeserializeMaterialGraph(std::string_view source,MaterialGraphAsset& asset,std::string& error);
bool ValidateMaterialGraph(const MaterialGraphAsset& asset,std::string& error);
bool SaveMaterialGraph(const MaterialGraphAsset& asset,const std::filesystem::path& path,std::string& error);
MaterialGraphAsset CreateDefaultMaterialGraph(std::string name="Material Graph");
MaterialGraphNode* FindMaterialGraphNode(MaterialGraphAsset& asset,uint64_t node);
const MaterialGraphNode* FindMaterialGraphNode(const MaterialGraphAsset& asset,uint64_t node);
uint64_t AddMaterialGraphNode(MaterialGraphAsset& asset,std::string operation,MaterialGraphType type=MaterialGraphType::Float);
std::string MaterialGraphBindingName(uint64_t parameter,bool isStatic=false);
std::string MaterialGraphTypeName(MaterialGraphType type);
MaterialGraphSource GenerateMaterialGraphShader(const MaterialGraphAsset& graph,const MaterialGraphFunctionProvider& functions={});
}
