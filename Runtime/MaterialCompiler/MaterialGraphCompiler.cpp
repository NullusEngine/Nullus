#include "MaterialGraph.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>
#include <regex>

namespace NLS::MaterialCompiler
{
namespace
{
using Type=MaterialGraphType;
struct Expression {std::string code;Type type=Type::Float;};
int Width(Type type)
{return type==Type::Float2?2:type==Type::Float3?3:type==Type::Float4?4:1;}
bool Numeric(Type type){return type<=Type::Bool;}
std::string Number(float value)
{std::ostringstream out;out.precision(9);out<<std::showpoint<<value;return out.str();}
std::string Literal(Type type,const std::array<float,4>& value)
{
    if(type==Type::Bool)return value[0]!=0?"true":"false";
    if(type==Type::Int)return std::to_string(static_cast<int32_t>(value[0]));
    if(type==Type::Float)return Number(value[0]);
    std::string text=MaterialGraphTypeName(type)+"(";
    for(int index=0;index<Width(type);++index){if(index)text+=',';text+=Number(value[index]);}
    return text+")";
}
std::string Convert(const Expression& expression,Type type,bool colorOutput=false)
{
    if(expression.type==type)return expression.code;
    if(colorOutput && expression.type==Type::Float4 && type==Type::Float3)return "("+expression.code+").rgb";
    if(expression.type==Type::Float && type>=Type::Float2 && type<=Type::Float4)
        return MaterialGraphTypeName(type)+"("+expression.code+")";
    throw std::runtime_error("Incompatible connection from "+MaterialGraphTypeName(expression.type)+" to "+MaterialGraphTypeName(type));
}
Render::Resources::UniformType Uniform(Type type)
{
    using U=Render::Resources::UniformType;
    switch(type)
    {case Type::Float2:return U::UNIFORM_FLOAT_VEC2;case Type::Float3:return U::UNIFORM_FLOAT_VEC3;
    case Type::Float4:return U::UNIFORM_FLOAT_VEC4;case Type::Int:return U::UNIFORM_INT;
    case Type::Bool:return U::UNIFORM_BOOL;case Type::Texture2D:return U::UNIFORM_SAMPLER_2D;
    case Type::TextureCube:return U::UNIFORM_SAMPLER_CUBE;default:return U::UNIFORM_FLOAT;}
}

class Emitter
{
public:
    Emitter(const MaterialGraphAsset& root,const MaterialGraphFunctionProvider& provider,MaterialGraphSource& output,bool vertex)
        :root(root),provider(provider),output(output),vertex(vertex){}
    Expression Emit(const MaterialGraphAsset& graph,const MaterialGraphLink& link,std::string scope="r",
        const std::map<uint64_t,Expression>& arguments={})
    {
        const auto* node=FindMaterialGraphNode(graph,link.node);
        if(!node)throw std::runtime_error("Missing node "+std::to_string(link.node));
        currentGraph=graph.id;currentNode=node->id;
        const auto key=scope+":"+std::to_string(node->id)+":"+std::to_string(link.output);
        if(auto cached=cache.find(key);cached!=cache.end())return cached->second;
        if(!visiting.insert(key).second)throw std::runtime_error("Material expression cycle");
        auto input=[&](std::string_view name,Type type,std::string fallback="0")
        {
            const auto found=node->inputs.find(std::string(name));
            return found==node->inputs.end() || !found->second.node?MaterialGraphTypeName(type)+"("+fallback+")":
                Convert(Emit(graph,found->second,scope,arguments),type);
        };
        auto raw=[&](std::string_view name)->Expression
        {
            const auto found=node->inputs.find(std::string(name));
            if(found==node->inputs.end() || !found->second.node)throw std::runtime_error("Missing required input "+std::string(name));
            return Emit(graph,found->second,scope,arguments);
        };
        Expression result;result.type=node->type;
        const auto& op=node->operation;
        if(op=="Constant")result.code=Literal(node->type,node->value);
        else if(op=="Parameter")
        {
            const auto parameter=std::find_if(root.parameters.begin(),root.parameters.end(),[&](const auto& item){return item.id==node->parameter;});
            if(parameter==root.parameters.end())throw std::runtime_error("Missing graph parameter");
            if(parameter->isStatic)throw std::runtime_error("Static booleans are evaluated by a StaticSwitch node");
            result={MaterialGraphBindingName(parameter->id),parameter->type};
        }
        else if(op=="FunctionInput")
        {
            const auto found=arguments.find(node->parameter);
            if(found==arguments.end())throw std::runtime_error("Function input has no matching argument");
            result=found->second;
        }
        else if(op=="Function")
        {
            const auto function=provider?provider(node->function):std::nullopt;
            if(!function || !function->function)throw std::runtime_error("Cannot resolve material function: "+node->function.Key());
            if(!functionStack.insert(function->id).second)throw std::runtime_error("Recursive material function dependency");
            std::string error;if(!ValidateMaterialGraph(*function,error))throw std::runtime_error(error);
            if(link.output>=function->functionOutputs.size())throw std::runtime_error("Function output no longer exists");
            std::map<uint64_t,Expression> supplied;
            for(const auto& port:function->functionInputs)
            {
                auto value=raw(port.name);Convert(value,port.type);supplied[port.id]={Convert(value,port.type),port.type};
            }
            const auto& port=function->functionOutputs[link.output];
            const auto selected=function->outputs.find(port.name);
            if(selected==function->outputs.end())throw std::runtime_error("Function output is not connected: "+port.name);
            result=Emit(*function,selected->second,scope+"f"+std::to_string(node->id),supplied);
            result={Convert(result,port.type),port.type};functionStack.erase(function->id);
            if(std::find(output.dependencies.begin(),output.dependencies.end(),node->function)==output.dependencies.end())output.dependencies.push_back(node->function);
        }
        else if(op=="StaticSwitch")
        {
            const auto parameter=std::find_if(root.parameters.begin(),root.parameters.end(),[&](const auto& item){return item.id==node->parameter && item.isStatic;});
            if(parameter==root.parameters.end())throw std::runtime_error("StaticSwitch needs a static boolean parameter");
            const auto preceding=std::move(code);code.clear();const auto cached=cache;
            const auto a=input("True",node->type);const auto trueCode=std::move(code);code.clear();cache=cached;
            const auto b=input("False",node->type);const auto falseCode=std::move(code);code=preceding;cache=cached;
            result.code="n"+std::to_string(serial++);
            code+=MaterialGraphTypeName(node->type)+" "+result.code+";\n#if defined("+MaterialGraphBindingName(parameter->id,true)+")\n"+
                trueCode+result.code+"="+a+";\n#else\n"+falseCode+result.code+"="+b+";\n#endif\n";
        }
        else if(op=="CustomHLSL")
        {
            static const std::regex resources(R"(\b(register|cbuffer|RWTexture\w*|RWStructuredBuffer|ByteAddressBuffer|RWByteAddressBuffer|groupshared)\b)");
            if(std::regex_search(node->code,resources))throw std::runtime_error("Custom HLSL resources must be exposed as typed graph inputs");
            const auto name=std::string(vertex?"GraphCustomV":"GraphCustomP")+std::to_string(serial++);
            std::string signature,call;
            for(const auto& port:node->customInputs)
            {if(!signature.empty()){signature+=',';call+=',';}signature+=MaterialGraphTypeName(port.type)+" "+port.name;
                call+=input(port.name,port.type);}
            for(const auto& include:node->includes)includes.insert(include);
            if(node->customOutputs.empty())
            {
                helpers+="#line 1 \""+graph.id+"#node"+std::to_string(node->id)+"\"\n"+
                    MaterialGraphTypeName(node->type)+" "+name+"("+signature+"){\n"+node->code+"\n}\n";
                result.code=name+"("+call+")";
            }
            else
            {
                std::vector<Expression> values;
                for(const auto& port:node->customOutputs)
                {
                    if(!signature.empty()){signature+=',';call+=',';}
                    signature+="out "+MaterialGraphTypeName(port.type)+" "+port.name;
                    Expression value{"n"+std::to_string(serial++),port.type};values.push_back(value);
                    code+=MaterialGraphTypeName(port.type)+" "+value.code+";\n";call+=value.code;
                }
                helpers+="#line 1 \""+graph.id+"#node"+std::to_string(node->id)+"\"\nvoid "+name+"("+signature+"){\n"+node->code+"\n}\n";
                code+=name+"("+call+");\n";
                for(size_t index=0;index<values.size();++index)cache[scope+":"+std::to_string(node->id)+":"+std::to_string(index)]=values[index];
                if(link.output>=values.size())throw std::runtime_error("Custom HLSL output no longer exists");result=values[link.output];
            }
        }
        else if(op=="UV")result={"ctx.uv",Type::Float2};
        else if(op=="WorldPosition")result={"ctx.positionWS",Type::Float3};
        else if(op=="WorldNormal")result={"ctx.normalWS",Type::Float3};
        else if(op=="ViewDirection")result={"ctx.viewDirection",Type::Float3};
        else if(op=="Time")
        {if(root.domain==MaterialGraphDomain::ParticleSprite)throw std::runtime_error("Use particle age instead of scene Time in ParticleSprite materials");result={"u_Time",Type::Float};}
        else if(op=="ParticleColor" || op=="ParticleBaseColor" || op=="ParticleAge" || op=="ParticleNormalizedAge" || op=="ParticleVelocity" || op=="DynamicParameter")
        {
            if(root.domain!=MaterialGraphDomain::ParticleSprite)throw std::runtime_error("Particle inputs require the ParticleSprite domain");
            if(op=="ParticleColor")result={"ctx.particleColor",Type::Float4};
            if(op=="ParticleBaseColor")result={"ctx.baseColor",Type::Float4};
            if(op=="ParticleAge")result={"ctx.age",Type::Float};
            if(op=="ParticleNormalizedAge")result={"ctx.normalizedAge",Type::Float};
            if(op=="ParticleVelocity")result={"ctx.velocity",Type::Float3};
            if(op=="DynamicParameter")
            {const auto index=static_cast<int>(node->value[0]);if(index<0 || index>3)throw std::runtime_error("Particle dynamic parameter index must be 0 to 3");
                result={"ctx.dynamicParameter"+std::to_string(index),Type::Float4};}
        }
        else if(op=="Sampler")result={"GraphLinearSampler",Type::Sampler};
        else if(op=="SampleTexture")
        {
            const auto texture=raw("Texture");
            if(texture.type!=Type::Texture2D && texture.type!=Type::TextureCube)throw std::runtime_error("Texture input is not a texture resource");
            const auto sampler=node->inputs.contains("Sampler")?Convert(raw("Sampler"),Type::Sampler):"GraphLinearSampler";
            const auto uv=input("UV",texture.type==Type::TextureCube?Type::Float3:Type::Float2,texture.type==Type::TextureCube?"0,0,1":"ctx.uv");
            const auto lod=node->inputs.contains("LOD") || vertex;
            result={texture.code+(lod?".SampleLevel(":".Sample(")+sampler+","+uv+(lod?","+input("LOD",Type::Float):std::string{})+")",Type::Float4};
        }
        else if(op=="Swizzle")
        {
            const auto value=raw("Value");const auto channels=node->name.empty()?"x":node->name;
            if(channels.size()>4 || channels.find_first_not_of("xyzwrgba")!=std::string::npos)throw std::runtime_error("Invalid swizzle");
            result={"("+value.code+")."+channels,static_cast<Type>(channels.size()-1)};
        }
        else if(op=="Append")
        {const auto a=raw("A"),b=raw("B");const auto width=Width(a.type)+Width(b.type);if(width>4)throw std::runtime_error("Append exceeds float4");
            result={MaterialGraphTypeName(static_cast<Type>(width-1))+"("+a.code+","+b.code+")",static_cast<Type>(width-1)};}
        else if(op=="Add" || op=="Subtract" || op=="Multiply" || op=="Divide")
        {const auto symbol=op=="Add"?"+":op=="Subtract"?"-":op=="Multiply"?"*":"/";
            result.code="("+input("A",node->type)+symbol+input("B",node->type,op=="Multiply" || op=="Divide"?"1":"0")+")";}
        else if(op=="Lerp")result.code="lerp("+input("A",node->type)+","+input("B",node->type,"1")+","+input("T",Type::Float,"0.5")+")";
        else if(op=="Clamp")result.code="clamp("+input("Value",node->type)+","+input("Min",node->type)+","+input("Max",node->type,"1")+")";
        else if(op=="Power")result.code="pow("+input("Base",node->type)+","+input("Exponent",node->type,"1")+")";
        else if(op=="OneMinus")result.code="(1-"+input("Value",node->type)+")";
        else if(op=="Saturate" || op=="Sine" || op=="Cosine" || op=="Frac" || op=="Abs")
        {const auto function=op=="Saturate"?"saturate":op=="Sine"?"sin":op=="Cosine"?"cos":op=="Frac"?"frac":"abs";
            result.code=std::string(function)+"("+input("Value",node->type)+")";}
        else if(op=="Normalize")
        {const auto value=input("Value",node->type);result.code="("+value+"/max(length("+value+"),1e-8))";}
        else if(op=="Length")result={"length("+raw("Value").code+")",Type::Float};
        else if(op=="Dot")
        {const auto a=raw("A"),b=raw("B");result={"dot("+a.code+","+Convert(b,a.type)+")",Type::Float};}
        else if(op=="Cross")result={"cross("+input("A",Type::Float3)+","+input("B",Type::Float3)+")",Type::Float3};
        else if(op=="Fresnel")
        {if(vertex)throw std::runtime_error("Fresnel requires fragment inputs");
            result={"pow(1-saturate(dot(normalize(ctx.normalWS),normalize(ctx.viewDirection))),"+input("Exponent",Type::Float,"5")+")",Type::Float};}
        else if(op=="Noise")result={"frac(sin(dot("+input("Position",Type::Float3,"ctx.positionWS")+",float3(12.9898,78.233,37.719)))*43758.5453)",Type::Float};
        else if(op=="NormalMap")result={"normalize(float3(("+input("Sample",Type::Float4,"0.5,0.5,1,1")+").xy*2-1,1))",Type::Float3};
        else if(op=="DDX" || op=="DDY")
        {if(vertex)throw std::runtime_error("Screen derivatives are unavailable in the Vertex stage");
            result.code=std::string(op=="DDX"?"ddx(":"ddy(")+input("Value",node->type)+")";}
        else throw std::runtime_error("Unknown material node operation: "+op);
        if(Numeric(result.type) && op!="StaticSwitch" && op!="FunctionInput" && op!="Constant")
        {
            const auto name="n"+std::to_string(serial++);
            code+=MaterialGraphTypeName(result.type)+" "+name+"="+result.code+";\n";
            result.code=name;
        }
        visiting.erase(key);cache[key]=result;return result;
    }
    std::string Output(std::string_view name,Type type,std::string fallback)
    {
        const auto link=root.outputs.find(std::string(name));
        return link==root.outputs.end() || !link->second.node?fallback:Convert(Emit(root,link->second),type,name=="BaseColor");
    }
    std::string code,helpers,currentGraph;
    std::set<std::string> includes;
    uint64_t currentNode=0;
private:
    const MaterialGraphAsset& root;
    const MaterialGraphFunctionProvider& provider;
    MaterialGraphSource& output;
    bool vertex;
    size_t serial=0;
    std::map<std::string,Expression> cache;
    std::set<std::string> visiting,functionStack;
};

std::string ContextDefinition()
{
    return R"(
struct GraphContext {float2 uv;float3 positionWS,normalWS,viewDirection,velocity;
float4 particleColor,baseColor,dynamicParameter0,dynamicParameter1,dynamicParameter2,dynamicParameter3;
float age,normalizedAge;};
struct GraphSurface {float3 baseColor,normalTS,emissive;float metallic,roughness,ao,opacity,cutoff;};
)";
}
std::string ContextFromVaryings()
{
    return R"(
GraphContext ctx=(GraphContext)0;ctx.uv=input.uv;ctx.positionWS=input.positionWS;ctx.normalWS=input.normalWS;
ctx.viewDirection=u_CameraWorldPos-input.positionWS;ctx.baseColor=1;ctx.particleColor=1;
)";
}
std::string SurfaceVertex(const MaterialGraphAsset& graph)
{
    return std::string(R"(
struct Attributes {float3 positionOS:POSITION;float2 uv:TEXCOORD0;float3 normalOS:NORMAL;float3 tangentOS:TEXCOORD1;float3 bitangentOS:TEXCOORD2;NLS_SKIN_ATTRIBUTES};
struct Varyings {float4 positionCS:SV_Position;float2 uv:TEXCOORD0;float3 positionWS:TEXCOORD1;
float3 normalWS:TEXCOORD2;float3 tangentWS:TEXCOORD3;float3 bitangentWS:TEXCOORD4;};
Varyings VSMain(Attributes input) {
Varyings output;float3 originalWS=TransformObjectToWorld(NLS_SKIN_POSITION(input));
NLSTangentFrame tangent=NLSBuildStandardPbrTangentFrame((float3x3)u_Model,NLS_SKIN_NORMAL(input),NLS_SKIN_TANGENT(input),NLS_SKIN_BITANGENT(input));
GraphContext ctx=(GraphContext)0;ctx.uv=input.uv;ctx.positionWS=originalWS;ctx.normalWS=tangent.normalWS;ctx.viewDirection=u_CameraWorldPos-originalWS;
float3 offset=EvaluateGraphVertex(ctx);
float maximum=)")+Number(graph.maximumWorldDisplacement)+R"(;
offset*=min(1,maximum/max(length(offset),1e-8));
output.positionWS=originalWS+offset;output.positionCS=TransformWorldToHClip(output.positionWS);
output.uv=input.uv;output.normalWS=tangent.normalWS;output.tangentWS=tangent.tangentWS;output.bitangentWS=tangent.bitangentWS;return output;}
)";
}
}

MaterialGraphSource GenerateMaterialGraphShader(const MaterialGraphAsset& graph,const MaterialGraphFunctionProvider& functions)
{
    MaterialGraphSource result;
    std::string error;
    if(!ValidateMaterialGraph(graph,error) || graph.function)
    {result.diagnostics.push_back({graph.id,0,{},graph.function?"A function must be called by a material graph":error});return result;}
    Emitter pixel(graph,functions,result,false),vertex(graph,functions,result,true);
    try
    {
        if(graph.outputs.contains("WorldPositionOffset") && graph.outputs.at("WorldPositionOffset").node &&
            (graph.domain==MaterialGraphDomain::ParticleSprite || graph.maximumWorldDisplacement<=0))
            throw std::runtime_error("World Position Offset requires the Surface domain and a positive maximum displacement");
        std::string properties,declarations,constants;
        uint32_t textureRegister=0;
        for(const auto& parameter:graph.parameters)
        {
            Render::Resources::MaterialParameterDesc desc;
            desc.id=parameter.id;desc.name=MaterialGraphBindingName(parameter.id,parameter.isStatic);desc.displayName=parameter.name;
            desc.type=Uniform(parameter.type);desc.defaultValue=parameter.value;desc.group=parameter.group;desc.color=parameter.color;
            desc.isStatic=parameter.isStatic;desc.range=parameter.range;result.schema.parameters.push_back(desc);
            if(parameter.isStatic)continue;
            const auto name=desc.name;
            if(parameter.type==Type::Texture2D || parameter.type==Type::TextureCube)
                declarations+=MaterialGraphTypeName(parameter.type)+" "+name+" : register(t"+std::to_string(textureRegister++)+",space2);\n";
            else constants+=MaterialGraphTypeName(parameter.type)+" "+name+";\n";
        }
        if(!constants.empty())declarations+="cbuffer GraphMaterialProperties : register(b0,space2) {\n"+constants+"};\n";
        declarations+="SamplerState GraphLinearSampler : register(s0,space2);\n";
        const auto base=pixel.Output("BaseColor",Type::Float3,graph.domain==MaterialGraphDomain::ParticleSprite?"ctx.baseColor.rgb":"float3(1,1,1)");
        const auto metallic=pixel.Output("Metallic",Type::Float,"0");
        const auto roughness=pixel.Output("Roughness",Type::Float,"1");
        const auto ao=pixel.Output("AmbientOcclusion",Type::Float,"1");
        const auto normal=pixel.Output("Normal",Type::Float3,"float3(0,0,1)");
        const auto emissive=pixel.Output("Emissive",Type::Float3,"float3(0,0,0)");
        const auto opacity=pixel.Output("Opacity",Type::Float,graph.domain==MaterialGraphDomain::ParticleSprite?"ctx.baseColor.a":"1");
        const auto cutoff=pixel.Output("AlphaCutoff",Type::Float,"0.5");
        const auto wpo=vertex.Output("WorldPositionOffset",Type::Float3,"float3(0,0,0)");
        std::string includes;
        for(const auto& include:pixel.includes)includes+="#include \""+include+"\"\n";
        for(const auto& include:vertex.includes)if(!pixel.includes.contains(include))includes+="#include \""+include+"\"\n";
        std::string evaluation=ContextDefinition()+pixel.helpers+vertex.helpers+
            "GraphSurface EvaluateGraphSurface(GraphContext ctx){\n"+pixel.code+"GraphSurface s;s.baseColor="+base+";s.metallic="+metallic+
            ";s.roughness="+roughness+";s.ao="+ao+";s.normalTS="+normal+";s.emissive="+emissive+";s.opacity="+opacity+";s.cutoff="+cutoff+";return s;}\n"+
            "float3 EvaluateGraphVertex(GraphContext ctx){\n"+vertex.code+"return "+wpo+";}\n";
        result.shader.shaderName=graph.name;
        Render::ShaderLab::ShaderLabSubShaderDesc sub;
        auto pass=[&](std::string role,std::string program)
        {
            Render::ShaderLab::ShaderLabPassDesc descriptor;
            descriptor.name=role;descriptor.tags.values["LightMode"]=role;
            descriptor.tags.values["MaterialDomain"]=graph.domain==MaterialGraphDomain::ParticleSprite?"ParticleSprite":"Surface";
            descriptor.tags.values["ShadingModel"]=graph.shading==MaterialGraphShading::PBR?"PBR":"Unlit";
            descriptor.tags.values["MaximumWorldDisplacement"]=Number(graph.maximumWorldDisplacement);
            descriptor.tags.values["MaterialRenderPath"]=graph.shading==MaterialGraphShading::Unlit ||
                (graph.outputs.contains("Emissive") && graph.outputs.at("Emissive").node)?"ForwardOpaque":"Deferred";
            descriptor.tags.values["BlendMode"]=std::to_string(static_cast<int>(graph.blend));
            descriptor.state.cullMode=graph.doubleSided?Render::ShaderLab::ShaderLabCullMode::Off:Render::ShaderLab::ShaderLabCullMode::Back;
            descriptor.state.depthWrite=graph.blend<=MaterialGraphBlend::Masked;
            descriptor.state.depthCompare=Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
            descriptor.state.blend.enabled=graph.blend>=MaterialGraphBlend::Alpha;
            if(descriptor.state.blend.enabled)
            {
                Render::RHI::RHIRenderTargetBlendStateDesc blend;blend.blendEnable=true;
                if(graph.blend==MaterialGraphBlend::Premultiplied)blend.srcColor=Render::RHI::RHIBlendFactor::One;
                if(graph.blend==MaterialGraphBlend::Additive)blend.dstColor=Render::RHI::RHIBlendFactor::One;
                descriptor.state.blend.renderTargets={blend};
            }
            for(const auto& parameter:graph.parameters)if(parameter.isStatic)
                descriptor.shaderFeatures.push_back({{MaterialGraphBindingName(parameter.id,true)}, {}});
            descriptor.vertexEntry=graph.domain==MaterialGraphDomain::Surface?"VSMain":std::string{};
            descriptor.fragmentEntry=graph.domain==MaterialGraphDomain::ParticleSprite?"ParticleMaterialPS":"PSMain";
            descriptor.hlslSource=std::move(program);sub.passes.push_back(std::move(descriptor));
        };
        const auto clip=graph.blend==MaterialGraphBlend::Masked?"clip(s.opacity-s.cutoff);":"";
        if(graph.domain==MaterialGraphDomain::ParticleSprite)
        {
            const auto program="#include \"NullusShaderLibrary/ParticleSprite.hlsl\"\n"+includes+declarations+evaluation+R"(
float4 EvaluateParticleMaterial(ParticleMaterialInput input) {
GraphContext ctx=(GraphContext)0;ctx.uv=input.uv;ctx.positionWS=input.worldPosition;ctx.velocity=input.velocity;
ctx.particleColor=input.particleColor;ctx.baseColor=input.baseColor;ctx.age=input.age;ctx.normalizedAge=input.normalizedAge;
ctx.dynamicParameter0=input.dynamicParameter0;ctx.dynamicParameter1=input.dynamicParameter1;
ctx.dynamicParameter2=input.dynamicParameter2;ctx.dynamicParameter3=input.dynamicParameter3;
GraphSurface s=EvaluateGraphSurface(ctx);return float4(s.baseColor+s.emissive,s.opacity);}
)";
            pass("ParticleSprite",program);
        }
        else
        {
            const std::string common="#include \"NullusShaderLibrary/Core.hlsl\"\n#define NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP\n#include \"CommonTypes.hlsli\"\n#undef NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP\n#include \"NullusShaderLibrary/StandardPBRSurface.hlsl\"\n"+
                includes+declarations+evaluation+SurfaceVertex(graph);
            const std::string lights=R"(
#include "LightGridCommon.hlsli"
StructuredBuffer<uint> u_ForwardLocalLightBuffer : register(t0,space1);
StructuredBuffer<uint> u_NumCulledLightsGrid : register(t1,space1);
StructuredBuffer<uint> u_CulledLightDataGrid : register(t2,space1);
)";
            std::string forward=common+(graph.shading==MaterialGraphShading::PBR?lights:std::string("#include \"LightGridCommon.hlsli\"\n"))+
                "float4 PSMain(Varyings input,bool front:SV_IsFrontFace):SV_Target0 {\n"+ContextFromVaryings()+
                "GraphSurface s=EvaluateGraphSurface(ctx);"+clip;
            if(graph.shading==MaterialGraphShading::PBR)
                forward+=R"(
float3 geometry=NLSOrientGeometryNormal(normalize(input.normalWS),front);
NLSTangentFrame tangent=NLSBuildSafeTangentFrame(input.normalWS,input.tangentWS,input.bitangentWS);
tangent=NLSOrientTangentFrameForFace(tangent,front);
float3 normal=NLSConstrainShadingNormalToGeometryHemisphere(NLSApplyTangentNormal(s.normalTS,tangent),geometry);
float3 lighting=NLSAccumulateClusteredLightingPBR(u_ForwardLocalLightBuffer,u_NumCulledLightsGrid,u_CulledLightDataGrid,
input.positionWS,geometry,normal,s.baseColor,saturate(s.metallic),saturate(s.roughness),saturate(s.ao));
return float4(NLSToneMapACES(lighting+s.emissive),saturate(s.opacity));})";
            else forward+="return float4(NLSToneMapACES(s.baseColor+s.emissive),saturate(s.opacity));}";
            pass("Forward",forward);
            if(graph.shading==MaterialGraphShading::PBR && (!graph.outputs.contains("Emissive") || !graph.outputs.at("Emissive").node) && graph.blend<=MaterialGraphBlend::Masked)
                pass("GBuffer",common+R"(
struct GBufferOutput{float4 Albedo:SV_Target0;float4 Normal:SV_Target1;float4 Material:SV_Target2;};
GBufferOutput PSMain(Varyings input,bool front:SV_IsFrontFace){)"+ContextFromVaryings()+
                    "GraphSurface s=EvaluateGraphSurface(ctx);"+clip+R"(
float3 geometry=NLSOrientGeometryNormal(normalize(input.normalWS),front);
NLSTangentFrame tangent=NLSOrientTangentFrameForFace(NLSBuildSafeTangentFrame(input.normalWS,input.tangentWS,input.bitangentWS),front);
float3 normal=NLSConstrainShadingNormalToGeometryHemisphere(NLSApplyTangentNormal(s.normalTS,tangent),geometry);
GBufferOutput result;NLSPackStandardPbrGBuffer(s.baseColor,geometry,normal,saturate(s.metallic),saturate(s.roughness),saturate(s.ao),1,
result.Albedo,result.Normal,result.Material);return result;})");
            const auto depth=common+"float4 PSMain(Varyings input):SV_Target0 {"+ContextFromVaryings()+"GraphSurface s=EvaluateGraphSurface(ctx);"+clip+"return 0;}";
            pass("DepthOnly",depth);pass("ShadowCaster",depth);
            pass("Picking",common+"cbuffer EditorPickingProperties:register(b1,space2){float4 u_Diffuse;};\nfloat4 PSMain(Varyings input):SV_Target0 {"+
                ContextFromVaryings()+"GraphSurface s=EvaluateGraphSurface(ctx);"+clip+"return u_Diffuse;}");
            pass("SelectionMask",common+"cbuffer EditorSelectionProperties:register(b1,space2){float u_ObjectId;float u_SelectionClassification;int u_SelectionOutlinePassMode;float _SelectionPadding;};\nfloat4 PSMain(Varyings input):SV_Target0 {"+ContextFromVaryings()+
                "GraphSurface s=EvaluateGraphSurface(ctx);"+clip+"return float4(u_ObjectId,u_SelectionOutlinePassMode==0?1:0,1,u_SelectionClassification);}");
        }
        result.shader.subShaders.push_back(std::move(sub));
        if(!result.schema.Validate(error))throw std::runtime_error(error);
    }
    catch(const std::exception& exception)
    {result.diagnostics.push_back({pixel.currentGraph.empty()?graph.id:pixel.currentGraph,pixel.currentNode,{},exception.what()});result.shader={};}
    return result;
}
}
