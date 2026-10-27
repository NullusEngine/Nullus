#include <gtest/gtest.h>

#include <limits>
#include <VFX/SystemInstance.h>
#include <VFXCompiler/VfxCompiler.h>

namespace
{
std::shared_ptr<NLS::VFX::CompiledSystem> MakeMixedSystem()
{
    using namespace NLS::VFX;
    auto asset = CreateDefaultSystem();
    auto gpu = CreateDefaultSystem().emitters[0]; gpu.target = SimulationTarget::GPU;
    asset.emitters.push_back(std::move(gpu));
    auto result = std::make_shared<CompiledSystem>();
    std::vector<Diagnostic> diagnostics;
    EXPECT_TRUE(CompileSystem(asset,*result,diagnostics));
    return result;
}
}

TEST(VfxSystemTests, MixedEmitterPublicationsRetainDataParametersAndResetEpochs)
{
    using namespace NLS::VFX;
    const auto program = MakeMixedSystem();
    SystemInstance system;
    std::string error;
    ASSERT_TRUE(system.Initialize(program,error)) << error;
    const auto initial = system.Capture();
    ASSERT_EQ(initial->cpu.size(),2u); ASSERT_NE(initial->cpu[0],nullptr); EXPECT_EQ(initial->cpu[1],nullptr);
    ASSERT_TRUE(system.Tick(0.1));
    const auto first = system.Capture();
    ASSERT_EQ(first->cpu[0]->Count(),10u);
    const auto position = first->cpu[0]->Read(0,0); const auto id = first->cpu[0]->Id(0);
    ASSERT_TRUE(system.SetParameter("User.Speed",{4,0,0,0}));
    const auto parameterFrame = system.Capture();
    EXPECT_FALSE(first->parameters.contains("User.Speed"));
    EXPECT_EQ(parameterFrame->parameters.at("User.Speed")[0],4);
    EXPECT_DOUBLE_EQ(parameterFrame->deltaTime,0);
    ASSERT_TRUE(system.Tick(0.2));
    EXPECT_EQ(system.Capture()->cpu[0]->Count(),30u);
    EXPECT_EQ(first->cpu[0]->Count(),10u); EXPECT_EQ(first->cpu[0]->Read(0,0),position); EXPECT_EQ(first->cpu[0]->Id(0),id);
    system.SetPaused(true);
    const auto paused = system.Capture();
    ASSERT_TRUE(system.Tick(0.5)); EXPECT_EQ(system.Capture(),paused); EXPECT_DOUBLE_EQ(system.Age(),0.3);
    system.Reset();
    const auto reset = system.Capture();
    EXPECT_GT(reset->resetRevision,first->resetRevision); EXPECT_DOUBLE_EQ(reset->age,0); EXPECT_EQ(reset->cpu[0]->Count(),0u);
    EXPECT_TRUE(reset->paused); EXPECT_TRUE(reset->spawning); EXPECT_EQ(reset->parameters.at("User.Speed")[0],4);
    system.SetPaused(false); ASSERT_TRUE(system.Tick(0.1));
    EXPECT_EQ(system.Capture()->cpu[0]->Id(0),id); EXPECT_EQ(system.Capture()->cpu[0]->Read(0,0),position);
    system.Deactivate(); EXPECT_FALSE(system.Capture()->spawning);
    ASSERT_TRUE(system.Tick(1)); ASSERT_TRUE(system.Tick(1)); ASSERT_TRUE(system.Tick(0.1)); EXPECT_EQ(system.Capture()->cpu[0]->Count(),0u);
    system.Reset(); ASSERT_TRUE(system.Tick(0.1));
    system.Deactivate(true); EXPECT_EQ(system.Capture()->cpu[0]->Count(),0u); EXPECT_GT(system.Capture()->clearRevision,first->clearRevision);
    EXPECT_GT(system.Capture()->sequence,first->sequence);
}

TEST(VfxSystemTests, FailedInputAndProgramReplacementKeepTheLastPublication)
{
    using namespace NLS::VFX;
    SystemInstance system;
    std::string error;
    ASSERT_TRUE(system.Initialize(MakeMixedSystem(),error)) << error;
    ASSERT_TRUE(system.Tick(0.1));
    const auto frame = system.Capture();
    EXPECT_FALSE(system.Tick(-0.1)); EXPECT_FALSE(system.Tick(std::numeric_limits<double>::infinity()));
    EXPECT_FALSE(system.SetParameter("",{})); EXPECT_FALSE(system.SetParameter("User.Bad",{std::numeric_limits<float>::quiet_NaN(),0,0,0}));
    EXPECT_FALSE(system.Initialize({},error));
    auto malformed = MakeMixedSystem(); malformed->emitters.pop_back();
    EXPECT_FALSE(system.Initialize(malformed,error));
    EXPECT_EQ(system.Capture(),frame);
    ASSERT_TRUE(system.Tick(0)); EXPECT_EQ(system.Capture(),frame);
}
