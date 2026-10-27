#include <gtest/gtest.h>
#include <Json/json.hpp>
#include <VFX/SystemInstance.h>
#include <VFXCompiler/VfxCompiler.h>
#include <VFXCompiler/VfxStackCompiler.h>

namespace
{
using namespace NLS::VFX;
std::shared_ptr<CompiledSystem> TimedProgram(SystemTimingSettings timing)
{
    auto source = CreateDefaultSystem(); source.timing = timing;
    auto program = std::make_shared<CompiledSystem>(); std::vector<Diagnostic> issues;
    EXPECT_TRUE(CompileSystem(source,*program,issues)); return program;
}
void SameParticles(const ParticleDataSet& a,const ParticleDataSet& b)
{
    ASSERT_EQ(a.Count(),b.Count()); ASSERT_EQ(a.Attributes().size(),b.Attributes().size());
    for (uint32_t i = 0; i < a.Count(); ++i)
    {
        ASSERT_EQ(a.Id(i),b.Id(i));
        for (uint32_t attribute = 0; attribute < a.Attributes().size(); ++attribute) EXPECT_EQ(a.Read(attribute,i),b.Read(attribute,i));
    }
}
}

TEST(VfxTiming, FixedTicksAccumulateAndBatchIndependentlyOfHostFrames)
{
    const auto program = TimedProgram({true,0.125,0,0.25});
    SystemInstance a,b; std::string error;
    ASSERT_TRUE(a.Initialize(program,error)); ASSERT_TRUE(b.Initialize(program,error));
    const auto initial = a.Capture(); ASSERT_TRUE(a.Tick(0.0625)); EXPECT_EQ(a.Capture(),initial);
    ASSERT_TRUE(a.Tick(0.3125));
    for (int i = 0; i < 12; ++i) ASSERT_TRUE(b.Tick(0.03125));
    EXPECT_DOUBLE_EQ(a.Age(),0.375); EXPECT_EQ(a.Capture()->simulationSteps,(std::vector<double>{0.125,0.125,0.125}));
    SameParticles(*a.Capture()->cpu[0],*b.Capture()->cpu[0]);
    const auto frozen = a.Capture(); a.SetPaused(true); ASSERT_TRUE(a.Tick(0.0625)); a.SetPaused(false);
    ASSERT_TRUE(a.Tick(0.0625)); EXPECT_DOUBLE_EQ(a.Age(),0.375);
    ASSERT_TRUE(a.Tick(0.0625)); EXPECT_DOUBLE_EQ(a.Age(),0.5);
    EXPECT_DOUBLE_EQ(frozen->age,0.375); EXPECT_EQ(frozen->cpu[0]->Count(),37u);
}

TEST(VfxTiming, WarmupUsesFixedPhaseAndResetsOnlyTheVisibleSystemAge)
{
    const auto program = TimedProgram({true,0.125,2,0.2});
    SystemInstance world; std::string error; ASSERT_TRUE(world.Initialize(program,error));
    ASSERT_TRUE(world.Tick(0));
    EXPECT_DOUBLE_EQ(world.Age(),0); EXPECT_DOUBLE_EQ(world.Capture()->simulationAge,0.375);
    EXPECT_EQ(world.Capture()->simulationSteps,(std::vector<double>{0.125,0.125,0.125}));
    CpuEmitter reference; ASSERT_TRUE(reference.Initialize(program->asset.emitters[0],program->emitters[0].spawn,program->emitters[0].update,error));
    const std::array<double,3> warmup{0.125,0.125,0.125}; ASSERT_TRUE(reference.TickSteps(warmup));
    SameParticles(*world.Capture()->cpu[0],reference.Data());
    const auto warm = world.Capture(); ASSERT_TRUE(world.Tick(0)); EXPECT_EQ(world.Capture(),warm);
    ASSERT_TRUE(world.Tick(0.1)); EXPECT_DOUBLE_EQ(world.Age(),0.125); EXPECT_DOUBLE_EQ(world.Capture()->simulationAge,0.5);
    world.Reset(); world.SetPaused(true); ASSERT_TRUE(world.Tick(0)); EXPECT_EQ(world.Capture()->cpu[0]->Count(),0u);
    world.SetPaused(false); ASSERT_TRUE(world.Tick(0)); SameParticles(*world.Capture()->cpu[0],*warm->cpu[0]);
    EXPECT_DOUBLE_EQ(world.Age(),0);
}

TEST(VfxTiming, CatchupCapDropsWholeExcessTicksAndResetClearsTheRemainder)
{
    SystemInstance world; std::string error; ASSERT_TRUE(world.Initialize(TimedProgram({true,0.001,0,0.25}),error));
    ASSERT_TRUE(world.Tick(0.2505)); EXPECT_NEAR(world.Age(),0.1,1e-12); EXPECT_EQ(world.Capture()->simulationSteps.size(),100u);
    ASSERT_TRUE(world.Tick(0.0005)); EXPECT_NEAR(world.Age(),0.101,1e-12);
    ASSERT_TRUE(world.Tick(0.0005)); world.Reset(); ASSERT_TRUE(world.Tick(0.0005)); EXPECT_DOUBLE_EQ(world.Age(),0);
    const auto frame = world.Capture(); EXPECT_FALSE(world.Tick(-1)); EXPECT_EQ(world.Capture(),frame);
    auto invalid = TimedProgram({}); invalid->asset.timing.fixedTickDelta = 0;
    EXPECT_FALSE(world.Initialize(invalid,error)); EXPECT_EQ(world.Capture(),frame);
}

TEST(VfxTiming, VariableWarmupUsesExactTicksAndCurrentOverridesBeforeFirstPublication)
{
    auto source = CreateDefaultSystem(); source.timing = {false,0.01667,3,0.0625};
    auto& graph = source.emitters[0].spawn; graph.nodes.clear();
    const auto input = AddNode(graph,Operation::Parameter,ValueType::Float3); FindNode(graph,input)->name = "User.Origin";
    const auto output = AddNode(graph,Operation::WriteAttribute,ValueType::Float3); FindNode(graph,output)->name = "Particles.Position"; FindNode(graph,output)->inputs = {input};
    auto program = std::make_shared<CompiledSystem>(); std::vector<Diagnostic> issues; ASSERT_TRUE(CompileSystem(source,*program,issues));
    SystemInstance world; std::string error; ASSERT_TRUE(world.Initialize(program,error)); ASSERT_TRUE(world.SetParameter("User.Origin",{2,3,4,0}));
    CpuEmitter reference; ASSERT_TRUE(reference.Initialize(source.emitters[0],program->emitters[0].spawn,program->emitters[0].update,error)); ASSERT_TRUE(reference.SetParameter("User.Origin",{2,3,4,0}));
    const std::array<double,3> steps{0.0625,0.0625,0.0625}; ASSERT_TRUE(reference.TickSteps(steps)); ASSERT_TRUE(world.Tick(0));
    SameParticles(*world.Capture()->cpu[0],reference.Data()); EXPECT_DOUBLE_EQ(world.Age(),0); EXPECT_DOUBLE_EQ(world.Capture()->simulationAge,0.1875);
    world.Reset(); world.Deactivate(true); ASSERT_TRUE(world.Tick(0)); EXPECT_EQ(world.Capture()->cpu[0]->Count(),0u);
    world.Activate(); ASSERT_TRUE(world.Tick(0)); EXPECT_EQ(world.Capture()->cpu[0]->Count(),0u);
    world.Reset(); ASSERT_TRUE(world.Tick(0)); SameParticles(*world.Capture()->cpu[0],reference.Data());
}

TEST(VfxTiming, VersionedTimingRoundTripsAndMalformedOrDisguisedDataIsRejectedAtomically)
{
    namespace author = NLS::VFX::Authoring; using Json = nlohmann::json;
    auto system = author::CreateSystem(); system.timing = {true,0.125,3,0.25};
    const auto source = Json::parse(author::Serialize(system)); EXPECT_EQ(source["version"],5); EXPECT_EQ(source["runtime"]["version"],3);
    author::System restored; std::string error; ASSERT_TRUE(author::Deserialize(source.dump(),restored,error)); EXPECT_EQ(restored.timing,system.timing);
    auto disguised = source; disguised["version"] = 4; EXPECT_FALSE(author::Deserialize(disguised.dump(),restored,error)); EXPECT_EQ(restored.timing,system.timing);
    auto malformed = source; malformed["runtime"]["timing"]["warmupTickCount"] = 1.5; EXPECT_FALSE(author::Deserialize(malformed.dump(),restored,error));
    auto program = TimedProgram(system.timing);
    auto cooked = Json::parse(SerializeCompiledSystem(*program, false));
    EXPECT_EQ(cooked["version"], 7);
    CompiledSystem decoded; ASSERT_TRUE(DeserializeCompiledSystem(cooked.dump(),decoded,error)); EXPECT_EQ(decoded.asset.timing,system.timing);
    cooked["version"] = 4; EXPECT_FALSE(DeserializeCompiledSystem(cooked.dump(),decoded,error)); EXPECT_EQ(decoded.asset.timing,system.timing);
    SystemTimingSettings timing; timing.warmupTickDelta = 0.125; ASSERT_TRUE(SetSystemWarmupTime(timing,0.49)); EXPECT_EQ(timing.warmupTickCount,3u); EXPECT_DOUBLE_EQ(timing.WarmupTime(),0.375);
    EXPECT_FALSE(SetSystemWarmupTime(timing,-1)); timing.warmupTickDelta = 0; ASSERT_TRUE(SetSystemWarmupTime(timing,1)); EXPECT_EQ(timing.warmupTickCount,0u);
    timing.fixedTick = true; timing.fixedTickDelta = 0.0001; timing.warmupTickDelta = 1; timing.warmupTickCount = 1;
    EXPECT_FALSE(ValidateSystemTiming(timing,error));
}
