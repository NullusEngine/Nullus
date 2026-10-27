#include <gtest/gtest.h>
#include <fstream>
#include "MaterialCompiler/MaterialGraph.h"
#include "MaterialCompiler/MaterialShaderCompiler.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"

TEST(MaterialGraphTests, SerializationKeepsStableParametersConnectionsAndEditorLayout)
{
    using namespace NLS::MaterialCompiler;
    auto graph=CreateDefaultMaterialGraph("Paint");
    graph.nodes[0].position={12,45};graph.comments[1]="Base surface";
    const auto id=graph.parameters[0].id;
    std::string error;MaterialGraphAsset decoded;
    ASSERT_TRUE(DeserializeMaterialGraph(SerializeMaterialGraph(graph),decoded,error))<<error;
    EXPECT_EQ(decoded.parameters[0].id,id);EXPECT_EQ(decoded.outputs.at("BaseColor"),graph.outputs.at("BaseColor"));
    EXPECT_EQ(decoded.nodes[0].position,graph.nodes[0].position);
    decoded.parameters[0].name="Renamed paint";
    const auto generated=GenerateMaterialGraphShader(decoded);
    ASSERT_TRUE(generated.Succeeded());
    EXPECT_EQ(generated.schema.parameters[0].id,id);
    EXPECT_EQ(generated.schema.parameters[0].displayName,"Renamed paint");
}

TEST(MaterialGraphTests, CompilerDiagnosesCyclesAndVertexDerivativeMisuse)
{
    using namespace NLS::MaterialCompiler;
    auto graph=CreateDefaultMaterialGraph();
    const auto cycle=AddMaterialGraphNode(graph,"Add",MaterialGraphType::Float3);
    graph.nodes.back().inputs["A"]={cycle,0};graph.outputs["BaseColor"]={cycle,0};
    auto result=GenerateMaterialGraphShader(graph);
    ASSERT_FALSE(result.Succeeded());EXPECT_NE(result.diagnostics[0].message.find("cycle"),std::string::npos);
    graph.outputs.erase("BaseColor");graph.nodes.back().operation="DDX";
    graph.nodes.back().inputs.clear();graph.maximumWorldDisplacement=1;
    graph.outputs["WorldPositionOffset"]={cycle,0};
    result=GenerateMaterialGraphShader(graph);ASSERT_FALSE(result.Succeeded());
    EXPECT_NE(result.diagnostics[0].message.find("Vertex"),std::string::npos);
}

TEST(MaterialGraphTests, FunctionCallsHaveTypedInputsAndPreserveDependencies)
{
    using namespace NLS::MaterialCompiler;
    auto function=CreateDefaultMaterialGraph("Tint function");function.function=true;function.parameters.clear();function.nodes.clear();function.outputs.clear();
    function.functionInputs={{1,"Value",MaterialGraphType::Float3}};function.functionOutputs={{2,"Result",MaterialGraphType::Float3}};
    const auto input=AddMaterialGraphNode(function,"FunctionInput",MaterialGraphType::Float3);function.nodes.back().parameter=1;
    function.outputs["Result"]={input,0};
    auto graph=CreateDefaultMaterialGraph();
    const auto call=AddMaterialGraphNode(graph,"Function",MaterialGraphType::Float3);
    graph.nodes.back().function={function.id,{},"Assets/Tint.shaderfunc"};
    graph.nodes.back().inputs["Value"]={};
    const auto constant=AddMaterialGraphNode(graph,"Constant",MaterialGraphType::Float3);graph.nodes.back().value={0.1f,0.4f,0.8f,0};
    FindMaterialGraphNode(graph,call)->inputs["Value"]={constant,0};graph.outputs["BaseColor"]={call,0};
    const auto result=GenerateMaterialGraphShader(graph,[&](const auto&)->std::optional<MaterialGraphAsset>{return function;});
    ASSERT_TRUE(result.Succeeded())<<result.diagnostics[0].message;
    ASSERT_EQ(result.dependencies.size(),1);EXPECT_EQ(result.dependencies[0].guid,function.id);
}

TEST(MaterialGraphTests, SurfaceGraphPassesCompileThroughUnifiedBackendForDxilAndSpirv)
{
    using namespace NLS::MaterialCompiler;
    if(NLS::Render::ShaderCompiler::GetCurrentShaderCompilerToolchainIdentity().compilerPath.empty())GTEST_SKIP()<<"DXC unavailable";
    auto graph=CreateDefaultMaterialGraph("Graph compiler fixture");
    const auto result=GenerateMaterialGraphShader(graph);
    ASSERT_TRUE(result.Succeeded());ASSERT_FALSE(result.shader.subShaders.empty());
    const auto root=std::filesystem::temp_directory_path()/("MaterialGraph_"+graph.id);
    std::filesystem::create_directories(root);
    for(const auto& pass:result.shader.subShaders[0].passes)
    {
        MaterialShaderCompileOptions options;
        options.sourcePath=root/"Graph.shadergraph";options.sourceIdentity="Assets/Graph.shadergraph";
        options.subAssetKey="shader:Graph/"+pass.name;options.generatedPath=root/(pass.name+".hlsl");
        options.includeDirectories={(std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine/Shaders").string()};
        options.generateGlsl=false;
        const auto artifact=CompileMaterialPass(pass,options,result.schema);
        ASSERT_FALSE(artifact.stages.empty());
        for(const auto& stage:artifact.stages)
            EXPECT_EQ(stage.output.status,NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)<<pass.name<<": "<<stage.output.diagnostics;
        EXPECT_EQ(artifact.parameterSchema.parameters[0].id,graph.parameters[0].id);
    }
    std::filesystem::remove_all(root);
}

TEST(MaterialGraphTests, ParticleGraphUsesTheExistingSpriteMaterialAbiOnBothTargets)
{
    using namespace NLS::MaterialCompiler;
    if(NLS::Render::ShaderCompiler::GetCurrentShaderCompilerToolchainIdentity().compilerPath.empty())GTEST_SKIP()<<"DXC unavailable";
    auto graph=CreateDefaultMaterialGraph("Particle Graph");graph.domain=MaterialGraphDomain::ParticleSprite;
    graph.shading=MaterialGraphShading::Unlit;graph.blend=MaterialGraphBlend::Alpha;
    const auto color=AddMaterialGraphNode(graph,"ParticleColor",MaterialGraphType::Float4);
    const auto dynamic=AddMaterialGraphNode(graph,"DynamicParameter",MaterialGraphType::Float4);graph.nodes.back().value[0]=1;
    const auto multiply=AddMaterialGraphNode(graph,"Multiply",MaterialGraphType::Float4);
    graph.nodes.back().inputs={{"A",{color,0}},{"B",{dynamic,0}}};graph.outputs["BaseColor"]={multiply,0};
    const auto generated=GenerateMaterialGraphShader(graph);ASSERT_TRUE(generated.Succeeded());
    ASSERT_EQ(generated.shader.subShaders.front().passes.size(),1);
    const auto root=std::filesystem::temp_directory_path()/("ParticleGraph_"+graph.id);
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}}cleanup{root};
    MaterialShaderCompileOptions options;options.sourcePath=root/"Particle.shadergraph";options.sourceIdentity="Assets/Particle.shadergraph";
    options.generatedPath=root/"Particle.hlsl";options.subAssetKey="shader:Particle";options.generateGlsl=false;
    options.includeDirectories={(std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine/Shaders").string()};
    const auto artifact=CompileMaterialPass(generated.shader.subShaders.front().passes.front(),options,generated.schema);
    for(const auto& stage:artifact.stages)ASSERT_EQ(stage.output.status,NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)<<stage.output.diagnostics;
    ASSERT_EQ(artifact.stages.size(),2);
    for(const auto& stage:artifact.stages)
    {EXPECT_EQ(stage.stage,NLS::Render::ShaderCompiler::ShaderStage::Pixel);
        EXPECT_EQ(stage.output.status,NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)<<stage.output.diagnostics;}
    EXPECT_EQ(artifact.shaderLabLightMode,"ParticleSprite");EXPECT_EQ(artifact.materialInterface.domain,"ParticleSprite");
}
