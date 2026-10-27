#include <gtest/gtest.h>
#include <Animation/Animation.h>
#include <Animation/Graph.h>
#include <AnimationCompiler/AnimationCompiler.h>
#include <Math/Vector4.h>
#include <numeric>

namespace
{
using namespace NLS::Animation;
namespace Compiler=NLS::AnimationCompiler;
std::shared_ptr<Skeleton> Rig()
{
    auto s=std::make_shared<Skeleton>();s->id="test-rig";
    s->bones={{"root","root",NoBone,{}},{"root/child","child",0,{{0,1,0},{},{1,1,1}}}};return s;
}
std::shared_ptr<Clip> Movement(float distance=2)
{
    auto c=std::make_shared<Clip>();c->id="move";c->name="Move";c->skeleton="test-rig";c->duration=1;
    BoneTrack t;t.bone="root";t.translation.times={0,1};t.translation.values={{0,0,0,0},{distance,0,0,0}};c->tracks.push_back(t);return c;
}
TEST(AnimationCore,SkeletonRejectsCyclesDuplicateIdsAndAmbiguousNames)
{
    auto s=Rig();std::string error;EXPECT_TRUE(s->Validate(error));s->bones[0].parent=1;EXPECT_FALSE(s->Validate(error));s=Rig();s->bones[1].id="root";EXPECT_FALSE(s->Validate(error));
    s=Rig();s->bones[1].name="root";EXPECT_EQ(s->Find("child"),NoBone);EXPECT_EQ(s->Find("root/child"),1u);
}
TEST(AnimationCore,SamplingLeavesMissingChannelsAtReference)
{
    auto s=Rig();auto c=Movement();Pose pose;SamplingContext context;std::string error;
    ASSERT_TRUE(Sample(*s,*c,.25,pose,context,error))<<error;EXPECT_NEAR(pose[0].translation.x,.5,1e-6);EXPECT_NEAR(pose[1].translation.y,1,1e-6);EXPECT_FLOAT_EQ(pose[1].rotation.w,1);
    std::vector<NLS::Maths::Matrix4> model;ASSERT_TRUE(LocalToModel(*s,pose,model,error));auto child=model[1]*NLS::Maths::Vector4(0,0,0,1);EXPECT_NEAR(child.x,.5,1e-6);EXPECT_NEAR(child.y,1,1e-6);
}
TEST(AnimationCore,StepAndCubicRespectInterpolationAndTangentTimeScale)
{
    Channel c;c.times={0,2};c.values={{0,0,0,0},{2,0,0,0}};c.interpolation=Interpolation::Step;EXPECT_FLOAT_EQ(c.Sample(1,{})[0],0);
    c.interpolation=Interpolation::CubicSpline;c.inTangents={{0,0,0,0},{0,0,0,0}};c.outTangents={{2,0,0,0},{0,0,0,0}};
    EXPECT_NEAR(c.Sample(1,{})[0],1.5,1e-6);EXPECT_FLOAT_EQ(c.Sample(-1,{})[0],0);EXPECT_FLOAT_EQ(c.Sample(3,{})[0],2);
}
TEST(AnimationCore,QuaternionAntipodesDoNotCollapse)
{
    Channel c;c.times={0,1};c.values={{0,0,0,1},{0,0,0,-1}};EXPECT_NEAR(std::abs(c.Sample(.5,{},true)[3]),1,1e-6);
}
TEST(AnimationCore,InvalidKeysAreRejected)
{
    Channel c;c.times={1,0};c.values={{},{}};std::string error;EXPECT_FALSE(c.Validate(error));c.times={0,1};c.interpolation=Interpolation::CubicSpline;EXPECT_FALSE(c.Validate(error));
}
TEST(AnimationCore,NegativeTimeWrapsAndZeroDurationHolds)
{EXPECT_DOUBLE_EQ(WrapTime(-.25,1),.75);EXPECT_DOUBLE_EQ(WrapTime(3,1),0);EXPECT_DOUBLE_EQ(WrapTime(100,0),0);}
TEST(AnimationCore,BindPoseSkinningIsIdentityForTranslatedMeshSpace)
{
    auto rig=Rig();auto pose=rig->ReferencePose();std::vector<NLS::Maths::Matrix4> model;std::string error;ASSERT_TRUE(LocalToModel(*rig,pose,model,error));
    SkinBinding binding;binding.joints={1};binding.meshFromSkeleton=NLS::Maths::Matrix4::Translation({-3,0,0});
    binding.inverseBind={NLS::Maths::Matrix4::Inverse(model[1])*NLS::Maths::Matrix4::Translation({3,0,0})};
    SkinningPalette palette;ASSERT_TRUE(BuildPalette(model,binding,palette,error));for(size_t i=0;i<16;++i)EXPECT_NEAR(palette.position[0].data[i],NLS::Maths::Matrix4::Identity.data[i],1e-5);
}
TEST(AnimationCore,InfluencesMergeTrimNormalizeAndRejectInvalidData)
{
    std::vector<std::pair<uint32_t,float>> weights{{0,.1f},{1,.2f},{2,.3f},{3,.4f},{4,.5f},{1,.1f}};
    Influence v;bool truncated;std::string error;ASSERT_TRUE(PackInfluences(weights,5,v,truncated,error));EXPECT_TRUE(truncated);EXPECT_EQ(v.joints[0],4u);EXPECT_EQ(std::accumulate(v.weights.begin(),v.weights.end(),0u),65535u);
    weights={{5,1}};EXPECT_FALSE(PackInfluences(weights,5,v,truncated,error));weights={{0,0}};EXPECT_FALSE(PackInfluences(weights,1,v,truncated,error));
}
TEST(AnimationCore,AdditiveUsesReferenceRotationAndTranslation)
{
    auto rig=Rig();auto base=rig->ReferencePose(),value=base,reference=base;value[0].translation.x=4;
    base[0].scale.x=3;reference[0].scale.x=2;value[0].scale.x=4;
    Pose out;ASSERT_TRUE(Additive(base,value,reference,.25,out));EXPECT_NEAR(out[0].translation.x,1,1e-6);EXPECT_NEAR(out[0].rotation.w,1,1e-6);EXPECT_NEAR(out[0].scale.x,3.75,1e-6);
}
TEST(AnimationCore,MultiBlendNormalizesQuaternionWeightsOnce)
{
    Pose a(1),b(1),c(1),output;b[0].rotation=NLS::Maths::Quaternion(NLS::Maths::Vector3{0,0,60});c[0].rotation=NLS::Maths::Quaternion(NLS::Maths::Vector3{0,0,120});
    a[0].translation.x=1;b[0].translation.x=2;c[0].translation.x=3;
    const std::array<const Pose*,3> poses{&a,&b,&c};const std::array<float,3> weights{2,3,5};ASSERT_TRUE(BlendWeightedPoses(poses,weights,output));
    auto expected=NLS::Maths::Quaternion::Normalize(a[0].rotation*.2f+b[0].rotation*.3f+c[0].rotation*.5f);
    EXPECT_NEAR(std::abs(NLS::Maths::Quaternion::DotProduct(output[0].rotation,expected)),1,1e-6);EXPECT_NEAR(output[0].translation.x,2.3,1e-6);
}
TEST(AnimationCore,MatrixTRSRoundTripIncludesNegativeAndNonuniformScale)
{
    Transform t{{2,3,4},NLS::Maths::Quaternion(NLS::Maths::Vector3{10,30,20}),{-2,3,4}},out;std::string error;ASSERT_TRUE(Decompose(ToMatrix(t),out,error))<<error;
    const auto a=ToMatrix(t),b=ToMatrix(out);for(size_t i=0;i<16;++i)EXPECT_NEAR(a.data[i],b.data[i],1e-4);
}
TEST(AnimationCore,NotifyIntervalsLoopReverseAndWeightThreshold)
{
    auto c=Movement();c->notifies={{1,"Step","",.25,.25,.01f},{2,"Hit","",.4,.6,.01f}};
    std::vector<NotifyEvent> events;CollectNotifies(*c,.2,1.3,42,1,true,events);ASSERT_EQ(events.size(),4u);EXPECT_EQ(events[0].phase,NotifyPhase::Instant);EXPECT_EQ(events.back().cycle,1);
    events.clear();CollectNotifies(*c,.8,.2,42,1,true,events);ASSERT_EQ(events.size(),3u);EXPECT_EQ(events[0].phase,NotifyPhase::Begin);EXPECT_EQ(events[1].phase,NotifyPhase::End);
    events.clear();CollectNotifies(*c,0,1,42,.001f,true,events);EXPECT_TRUE(events.empty());
}
TEST(AnimationCore,ModelArtifactRoundTripsAndRejectsCorruption)
{
    Model model;model.skeleton=*Rig();model.clips={*Movement()};std::string error;auto bytes=SerializeModel(model);auto loaded=DeserializeModel(bytes,error);ASSERT_TRUE(loaded)<<error;EXPECT_EQ(loaded->skeleton.Signature(),model.skeleton.Signature());EXPECT_DOUBLE_EQ(loaded->clips[0].duration,1);
    bytes.resize(7);EXPECT_FALSE(DeserializeModel(bytes,error));
}
TEST(AnimationGraph,CompilerRejectsPoseCyclesButPrunesUnreachableNodes)
{
    Compiler::Resources resources;resources.skeleton=Rig();auto graph=Compiler::CreateGraph();auto compiled=Compiler::Compile(graph,resources);ASSERT_TRUE(compiled.Succeeded());
    Node unused;unused.id=3;unused.kind=NodeKind::SequencePlayer;unused.clip="missing";graph.nodes.push_back(unused);compiled=Compiler::Compile(graph,resources);ASSERT_TRUE(compiled.Succeeded());EXPECT_EQ(compiled.program->nodes.size(),2u);
    graph.nodes[0].kind=NodeKind::SaveCachedPose;graph.nodes[0].inputs={2};compiled=Compiler::Compile(graph,resources);EXPECT_FALSE(compiled.Succeeded());
}
TEST(AnimationGraph,PlaybackPauseReverseSeekAndInstanceIsolation)
{
    auto rig=Rig();auto clip=Movement();AnimationInstance a,b;std::string error;ASSERT_TRUE(a.PlayClip(rig,clip,error));ASSERT_TRUE(b.PlayClip(rig,clip,error));
    ASSERT_TRUE(a.Tick(.25,error));EXPECT_NEAR(a.Capture()->local[0].translation.x,.5,1e-6);EXPECT_NEAR(b.Capture()->local[0].translation.x,0,1e-6);
    a.Pause();ASSERT_TRUE(a.Tick(.2,error));EXPECT_NEAR(a.Age(),.25,1e-6);a.Pause(false);a.SetSpeed(-1);ASSERT_TRUE(a.Tick(.5,error));EXPECT_NEAR(a.Capture()->local[0].translation.x,1.5,1e-6);
    a.Seek(.75);EXPECT_NEAR(a.Capture()->local[0].translation.x,1.5,1e-6);EXPECT_TRUE(a.Capture()->notifies.empty());
}
TEST(AnimationGraph,BlendParametersAreTypedAndAppliedAsSnapshot)
{
    Compiler::Resources resources;resources.skeleton=Rig();resources.clips["move"]=Movement();auto graph=Compiler::CreateGraph();
    graph.parameters={{10,"Weight",ParameterType::Float,{.5f,0,0,0}}};Node player;player.id=3;player.kind=NodeKind::SequenceEvaluator;player.clip="move";player.sampleTime=1;graph.nodes.push_back(player);
    Node blend;blend.id=4;blend.kind=NodeKind::Blend;blend.inputs={1,3};blend.parameter="Weight";graph.nodes.push_back(blend);graph.nodes[1].inputs={4};
    auto compiled=Compiler::Compile(graph,resources);ASSERT_TRUE(compiled.Succeeded());AnimationInstance instance;std::string error;ASSERT_TRUE(instance.SetProgram(compiled.program,error));
    EXPECT_FALSE(instance.SetBool("Weight",true));ASSERT_TRUE(instance.SetFloat("Weight",.25));ASSERT_TRUE(instance.Tick(0,error));
    // Evaluators sample the endpoint when explicitly configured not to loop.
    graph.nodes[2].loop=false;compiled=Compiler::Compile(graph,resources);ASSERT_TRUE(compiled.Succeeded());ASSERT_TRUE(instance.SetProgram(compiled.program,error));ASSERT_TRUE(instance.Tick(0,error));EXPECT_NEAR(instance.Capture()->local[0].translation.x,.5,1e-6);
}
TEST(AnimationGraph,BlendSpaceTriangulationAndBoundaryWeights)
{
    Compiler::Resources resources;resources.skeleton=Rig();resources.clips["move"]=Movement();BlendSpaceAsset asset;asset.dimensions=2;asset.samples={{"move",0,0},{"move",1,0},{"move",0,1}};
    std::string error;auto blend=Compiler::CompileBlendSpace(asset,resources,error);ASSERT_TRUE(blend)<<error;ASSERT_EQ(blend->triangles.size(),1u);
    auto weights=CalculateBlendWeights(*blend,.25,.25);float sum=0;for(auto w:weights)sum+=w.weight;EXPECT_NEAR(sum,1,1e-6);ASSERT_EQ(weights.size(),3u);
    weights=CalculateBlendWeights(*blend,3,3);sum=0;for(auto w:weights){sum+=w.weight;EXPECT_GE(w.weight,0);}EXPECT_NEAR(sum,1,1e-6);
}
TEST(AnimationGraph,StateMachineConsumesTriggerAndCrossfades)
{
    Compiler::Resources resources;resources.skeleton=Rig();resources.clips["move"]=Movement();auto graph=Compiler::CreateGraph();graph.parameters={{10,"Jump",ParameterType::Trigger,{}}};
    Node player;player.id=3;player.kind=NodeKind::SequenceEvaluator;player.clip="move";player.sampleTime=1;player.loop=false;graph.nodes.push_back(player);
    StateMachine machine;machine.id=100;machine.initial=1;machine.states={{1,"Idle",1},{2,"Jump",3}};Transition t;t.id=1;t.from=1;t.to=2;t.duration=.2;t.condition.parameter="Jump";machine.transitions.push_back(t);graph.machines.push_back(machine);
    Node state;state.id=4;state.kind=NodeKind::StateMachine;state.machine=100;graph.nodes.push_back(state);graph.nodes[1].inputs={4};
    auto compiled=Compiler::Compile(graph,resources);ASSERT_TRUE(compiled.Succeeded());AnimationInstance instance;std::string error;ASSERT_TRUE(instance.SetProgram(compiled.program,error));ASSERT_TRUE(instance.SetTrigger("Jump"));ASSERT_TRUE(instance.Tick(.1,error));ASSERT_TRUE(instance.Tick(.1,error));EXPECT_NEAR(instance.Capture()->local[0].translation.x,1,1e-5);EXPECT_EQ(instance.Capture()->activeStates[0],2u);
}
TEST(AnimationGraph,CompiledProgramEmbedsDependenciesAndRoundTrips)
{
    Compiler::Resources resources;resources.skeleton=Rig();resources.clips["move"]=Movement();auto graph=Compiler::CreateGraph();graph.nodes[0].kind=NodeKind::SequencePlayer;graph.nodes[0].clip="move";
    auto compiled=Compiler::Compile(graph,resources);ASSERT_TRUE(compiled.Succeeded());auto bytes=Compiler::SerializeProgram(*compiled.program);std::string error;auto loaded=Compiler::DeserializeProgram(bytes,error);ASSERT_TRUE(loaded)<<error;
    AnimationInstance instance;ASSERT_TRUE(instance.SetProgram(loaded,error));ASSERT_TRUE(instance.Tick(.25,error));EXPECT_NEAR(instance.Capture()->local[0].translation.x,.5,1e-6);
}
TEST(AnimationGraph,FailedProgramReplacementPreservesPublishedPose)
{
    AnimationInstance instance;std::string error;ASSERT_TRUE(instance.PlayClip(Rig(),Movement(),error));ASSERT_TRUE(instance.Tick(.2,error));auto frame=instance.Capture();EXPECT_FALSE(instance.SetProgram({},error));EXPECT_EQ(instance.Capture(),frame);
}
TEST(AnimationGraph,TrimmedCubicQuaternionPreservesTheOriginalMotion)
{
    Compiler::Resources resources;resources.skeleton=Rig();auto clip=Movement();clip->duration=2;
    auto& rotation=clip->tracks[0].rotation;rotation.interpolation=Interpolation::CubicSpline;rotation.times={0,2};rotation.values={{0,0,0,1},{0,0,.8f,.6f}};
    rotation.inTangents={{0,0,0,0},{0,0,.3f,-.1f}};rotation.outTangents={{0,0,.7f,.1f},{0,0,0,0}};resources.clips["original"]=clip;
    ClipAsset asset;asset.id="cropped";asset.source="original";asset.start=.3;asset.end=1.4;std::string error;auto trimmed=Compiler::CompileClip(asset,resources,error);ASSERT_TRUE(trimmed)<<error;
    for(double t:{0.,.2,.5,1.1})
    {const auto expected=rotation.Sample(t+asset.start,{},true);const auto actual=trimmed->tracks[0].rotation.Sample(t,{},true);for(size_t i=0;i<4;++i)EXPECT_NEAR(actual[i],expected[i],1e-5);}
}
TEST(AnimationGraph,EvaluationFailureDuringApplyPreservesTheOldInstance)
{
    AnimationInstance instance;std::string error;ASSERT_TRUE(instance.PlayClip(Rig(),Movement(),error));ASSERT_TRUE(instance.Tick(.2,error));auto frame=instance.Capture();
    Compiler::Resources resources;auto rig=Rig();rig->bones[0].reference.scale.x=0;resources.skeleton=rig;auto graph=Compiler::CreateGraph();
    Node additive;additive.id=3;additive.kind=NodeKind::Additive;additive.inputs={1,1};graph.nodes.push_back(additive);graph.nodes[1].inputs={3};
    auto compiled=Compiler::Compile(graph,resources);ASSERT_TRUE(compiled.Succeeded());EXPECT_FALSE(instance.SetProgram(compiled.program,error));EXPECT_EQ(instance.Capture(),frame);EXPECT_NEAR(instance.Age(),.2,1e-6);
}
TEST(AnimationGraph,NotifyStatesCleanUpOnSeekStopAndAcrossLoopBoundaries)
{
    auto clip=Movement();clip->notifies={{1,"Window","",0,1,.01f}};AnimationInstance instance;std::string error;ASSERT_TRUE(instance.PlayClip(Rig(),clip,error));
    ASSERT_TRUE(instance.Tick(.2,error));ASSERT_EQ(instance.Capture()->notifies.size(),1u);EXPECT_EQ(instance.Capture()->notifies[0].phase,NotifyPhase::Begin);
    instance.Seek(.5);EXPECT_TRUE(instance.Capture()->notifies.empty());ASSERT_TRUE(instance.Tick(0,error));ASSERT_EQ(instance.Capture()->notifies.size(),1u);EXPECT_EQ(instance.Capture()->notifies[0].phase,NotifyPhase::End);
    ASSERT_TRUE(instance.Tick(.1,error));ASSERT_EQ(instance.Capture()->notifies.size(),1u);EXPECT_EQ(instance.Capture()->notifies[0].phase,NotifyPhase::Begin);
    ASSERT_TRUE(instance.Tick(.5,error));ASSERT_EQ(instance.Capture()->notifies.size(),2u);EXPECT_EQ(instance.Capture()->notifies[0].phase,NotifyPhase::End);EXPECT_EQ(instance.Capture()->notifies[1].phase,NotifyPhase::Begin);
    instance.Stop();ASSERT_TRUE(instance.Tick(0,error));ASSERT_EQ(instance.Capture()->notifies.size(),1u);EXPECT_EQ(instance.Capture()->notifies[0].phase,NotifyPhase::End);
}
}
