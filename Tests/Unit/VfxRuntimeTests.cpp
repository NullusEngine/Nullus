#include "VfxTestExecution.h"
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <thread>
#include <vector>
#include <Guid.h>
#include <Jobs/JobSystem.h>
#include <VFX/CpuEmitter.h>
#include <VFXCompiler/VfxCompiler.h>
#include <Json/json.hpp>

namespace
{
using namespace NLS::VFX;

CpuEmitter MakeEmitter(EmitterAsset asset)
{
    auto spawn = CompileGraph(asset.spawn,asset.attributes), update = CompileGraph(asset.update,asset.attributes);
    EXPECT_TRUE(spawn.Succeeded());
    EXPECT_TRUE(update.Succeeded());
    CpuEmitter emitter;
    std::string error;
    EXPECT_TRUE(emitter.Initialize(asset,std::move(spawn.compiled.program),std::move(update.compiled.program),error)) << error;
    return emitter;
}

/**
 * Owns a JobSystem worker pool for the parallel-fan-out coverage.
 *
 * Emitter fan-out uses the general Jobs pool and nothing else; without one the
 * emitter deliberately falls back to the serial range path, so a determinism
 * test that skipped this would compare two serial runs and pass for the wrong
 * reason.
 */
class ScopedVfxJobSystem final
{
public:
    explicit ScopedVfxJobSystem(const uint32_t workerCount)
    {
        NLS::Base::Jobs::JobSystemConfig config;
        config.workerCount = workerCount;
        config.backgroundWorkerCount = 1u;
        m_initialized = NLS::Base::Jobs::TryInitializeJobSystem(config);
    }

    ~ScopedVfxJobSystem()
    {
        if (!m_initialized) return;
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
    }

    [[nodiscard]] bool Initialized() const { return m_initialized; }

private:
    bool m_initialized = false;
};

EmitterAsset MakeParallelBurstAsset(const uint32_t particles)
{
    auto asset = CreateDefaultSystem().emitters[0];
    asset.capacity = particles;
    asset.initialCapacity = 0;
    asset.spawnRate = 0;
    asset.duration = 1.0e6;
    asset.looping = true;
    asset.interpolatedSpawning = false;
    asset.bursts = {{0,particles}};
    for (auto& attribute : asset.attributes)
        if (attribute.name == "Particles.Lifetime") attribute.defaultValue = {1.0e6f,0,0,0};
    return asset;
}

void ExpectSameEmitterState(const CpuEmitter& serial, const CpuEmitter& parallel)
{
    const auto& expected = serial.Data();
    const auto& actual = parallel.Data();
    ASSERT_EQ(actual.Count(),expected.Count());
    ASSERT_EQ(actual.Attributes().size(),expected.Attributes().size());
    for (uint32_t particle = 0; particle < expected.Count(); ++particle)
    {
        ASSERT_EQ(actual.Id(particle),expected.Id(particle)) << "id mismatch at particle " << particle;
        for (size_t attribute = 0; attribute < expected.Attributes().size(); ++attribute)
            EXPECT_EQ(actual.Read(static_cast<uint32_t>(attribute),particle),
                expected.Read(static_cast<uint32_t>(attribute),particle)) << "attribute "
                << expected.Attributes()[attribute].name << " mismatch at particle " << particle;
    }
    EXPECT_EQ(parallel.Statistics().spawned,serial.Statistics().spawned);
    EXPECT_EQ(parallel.Statistics().died,serial.Statistics().died);
    EXPECT_EQ(parallel.Statistics().dropped,serial.Statistics().dropped);
    EXPECT_EQ(parallel.Statistics().simulationSteps,serial.Statistics().simulationSteps);
    EXPECT_EQ(parallel.Statistics().collisions,serial.Statistics().collisions);
    ASSERT_EQ(parallel.Events().size(),serial.Events().size());
    for (size_t event = 0; event < serial.Events().size(); ++event)
    {
        EXPECT_EQ(parallel.Events()[event].kind,serial.Events()[event].kind);
        EXPECT_EQ(parallel.Events()[event].particleId,serial.Events()[event].particleId);
        EXPECT_EQ(parallel.Events()[event].position,serial.Events()[event].position);
    }
}
}

TEST(VfxRuntimeTests, AssetRoundTripPreservesGraphAndFailedLoadPreservesPriorAsset)
{
    auto asset = CreateDefaultSystem("Round Trip");
    asset.emitters[0].sortByViewDepth = true;
    asset.emitters[0].frustumCulling = false;
    const auto text = SerializeSystem(asset);
    SystemAsset restored;
    std::string error;
    ASSERT_TRUE(DeserializeSystem(text,restored,error)) << error;
    EXPECT_TRUE(restored.emitters[0].sortByViewDepth);
    EXPECT_FALSE(restored.emitters[0].frustumCulling);
    EXPECT_EQ(SerializeSystem(restored),text);
    EXPECT_FALSE(DeserializeSystem("{\"version\":9}",restored,error));
    EXPECT_EQ(SerializeSystem(restored),text);
}

TEST(VfxRuntimeTests, LayoutDoesNotChangeRuntimeSource)
{
    auto asset = CreateDefaultSystem();
    const auto source = SerializeSystem(asset,false);
    asset.emitters[0].update.nodes[0].position = {44,99};
    EXPECT_EQ(source,SerializeSystem(asset,false));
}

TEST(VfxRuntimeTests, SpriteSettingsRoundTripAndInvalidSettingsPreserveThePreviousAsset)
{
    auto asset = CreateDefaultSystem();
    auto& sprite = asset.emitters[0].sprite;
    sprite.material = {NLS::Guid::New().ToString(),17}; sprite.texture = {NLS::Guid::New().ToString(),29};
    sprite.blend = SpriteBlendMode::Premultiplied; sprite.alignment = SpriteAlignment::Velocity;
    sprite.tint = {0.3f,0.5f,2,0.4f}; sprite.emissive = 4; sprite.alphaCutoff = 0.2f; sprite.softDistance = 0.3f;
    sprite.subImagesX = 4; sprite.subImagesY = 8; sprite.blendSubImages = true; sprite.circularMask = false;
    sprite.useMaterialShader = true;
    sprite.dynamicParameters[2] = {"Particles.CustomDynamic","User.Fallback",{0.2f,0.3f,0.4f,0.5f}};
    sprite.materialParameters = {{"_Tint","User.Tint",ValueType::Float4,{1,0.5f,0.2f,1}}};
    const auto text = SerializeSystem(asset);
    SystemAsset restored; std::string error;
    ASSERT_TRUE(DeserializeSystem(text,restored,error)) << error; EXPECT_EQ(SerializeSystem(restored),text);
    for (const auto& [key,value] : std::vector<std::pair<std::string,nlohmann::json>>{
        {"blend",255},{"alignment",256},{"softDistance",-1},{"subImagesX",0},{"subImagesY",257},
        {"alphaCutoff",1.1},{"emissive",-2},{"material",{{"guid","invalid"},{"localId",1}}},
        {"dynamicParameters",nlohmann::json::array()},{"materialParameters",nlohmann::json::array({{{"materialParameter","_Tint"},{"systemParameter","User.Tint"},{"type",256},{"default",{1,1,1,1}}}})},
        {"texture",{{"guid",sprite.texture.guid},{"localId",0}}}})
    {
        auto broken = nlohmann::json::parse(text); broken["emitters"][0]["sprite"][key] = value;
        EXPECT_FALSE(DeserializeSystem(broken.dump(),restored,error)) << key;
        EXPECT_EQ(SerializeSystem(restored),text);
    }
    sprite.blend = SpriteBlendMode::Masked;
    CompiledSystem compiled; std::vector<Diagnostic> diagnostics;
    EXPECT_FALSE(CompileSystem(asset,compiled,diagnostics)); ASSERT_FALSE(diagnostics.empty());
    EXPECT_NE(diagnostics[0].message.find("Soft particles"),std::string::npos);
    auto legacy = nlohmann::json::parse(text); legacy["emitters"][0].erase("sprite");
    ASSERT_TRUE(DeserializeSystem(legacy.dump(),restored,error));
    EXPECT_TRUE(restored.emitters[0].sprite.material.Empty()); EXPECT_TRUE(restored.emitters[0].sprite.circularMask);
    EXPECT_EQ(restored.emitters[0].sprite.subImagesX,1u);
    auto attributes = asset.emitters[0].attributes;
    attributes.push_back({"Particles.SubImageIndex",ValueType::Float3,{}});
    std::string source; EXPECT_FALSE(BuildSpriteShader(attributes,source,error));
    attributes = CreateDefaultSystem().emitters[0].attributes;
    attributes.push_back({"Particles.DynamicMaterialParameter",ValueType::Float,{}});
    EXPECT_FALSE(BuildSpriteShader(attributes,source,error));
}

TEST(VfxRuntimeTests, DataSetGrowthPreservesAttributesIdsAndFrozenCopies)
{
    auto attributes = CreateDefaultSystem().emitters[0].attributes;
    ParticleDataSet data; data.Initialize(attributes,1025,3);
    EXPECT_EQ(data.AllocatedCapacity(),3u); EXPECT_EQ(data.Capacity(),1025u);
    for (uint32_t index = 0; index < 259; ++index)
    { ASSERT_TRUE(data.Spawn(uint64_t(index)+(uint64_t(9)<<32))); data.Write(0,index,{float(index),-float(index),0.5f,0}); }
    const auto frozen = data;
    EXPECT_EQ(frozen.AllocatedCapacity(),frozen.Count());
    EXPECT_GE(data.AllocatedCapacity(),259u); EXPECT_LT(data.AllocatedCapacity(),data.Capacity());
    ASSERT_TRUE(data.Reserve(1025)); EXPECT_EQ(data.AllocatedCapacity(),1025u); EXPECT_EQ(data.Count(),259u);
    EXPECT_FALSE(data.Reserve(1026)); EXPECT_EQ(data.AllocatedCapacity(),1025u);
    data.Write(0,0,{999,0,0,0}); EXPECT_FLOAT_EQ(frozen.Read(0,0)[0],0);
    for (uint32_t index = 1; index < 259; ++index)
    { EXPECT_EQ(data.Id(index),frozen.Id(index)); EXPECT_EQ(data.Read(0,index),frozen.Read(0,index)); }
    while (data.Count() < data.Capacity()) ASSERT_TRUE(data.Spawn(data.Count()));
    EXPECT_FALSE(data.Spawn(0)); EXPECT_EQ(frozen.Count(),259u);
    data.Clear(); EXPECT_EQ(data.AllocatedCapacity(),1025u); EXPECT_EQ(data.Count(),0u);
}

TEST(VfxRuntimeTests, SimulationStageProgramsRoundTripAndValidateTargetAndContext)
{
    auto asset = CreateDefaultSystem(); auto& emitter = asset.emitters[0];
    emitter.target = SimulationTarget::GPU; emitter.initialCapacity = 3;
    auto stage = CreateSimulationStage("Iterated size"); stage.iterations = 3;
    const auto iteration = AddNode(stage.graph,Operation::StageIteration);
    const auto write = AddNode(stage.graph,Operation::WriteAttribute); FindNode(stage.graph,write)->name = "Particles.Size";
    FindNode(stage.graph,write)->inputs = {iteration};
    emitter.simulationStages = {stage};
    auto disabled = CreateSimulationStage("Disabled"); disabled.enabled = false; emitter.simulationStages.push_back(disabled);
    SystemAsset restored; std::string error;
    const auto source = SerializeSystem(asset); ASSERT_TRUE(DeserializeSystem(source,restored,error)) << error;
    EXPECT_EQ(SerializeSystem(restored),source); EXPECT_EQ(restored.emitters[0].initialCapacity,3u);
    CompiledSystem compiled; std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(asset,compiled,diagnostics));
    ASSERT_EQ(compiled.emitters[0].stages.size(),2u);
    CompiledSystem loaded;
    ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(compiled,false),loaded,error)) << error;
    EXPECT_TRUE(loaded.asset.emitters[0].simulationStages[0].graph.nodes.empty());
    EXPECT_TRUE(loaded.emitters[0].stages[0].hlsl.empty()); EXPECT_FALSE(loaded.asset.emitters[0].simulationStages[1].enabled);
    ParticleDataSet data; data.Initialize(emitter.attributes,3); ASSERT_TRUE(data.Spawn(42)); std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(loaded.emitters[0].stages[0].program, data, 0, 1, {0.1f, 1, 7, nullptr, 0, 2}, registers));
    EXPECT_FLOAT_EQ(data.Read(3,0)[0],2);
    auto invalid = asset; invalid.emitters[0].target = SimulationTarget::CPU;
    EXPECT_FALSE(CompileSystem(invalid,compiled,diagnostics));
    invalid = asset; invalid.emitters[0].simulationStages[0].iterations = 129;
    EXPECT_FALSE(CompileSystem(invalid,compiled,diagnostics));
    invalid = asset; invalid.emitters[0].initialCapacity = invalid.emitters[0].capacity+1;
    EXPECT_FALSE(CompileSystem(invalid,compiled,diagnostics));
    auto graph = stage.graph; graph.stage = ScriptStage::ParticleUpdate;
    EXPECT_FALSE(CompileGraph(graph,emitter.attributes).Succeeded());
}

TEST(VfxRuntimeTests, DynamicBoundsTrackMotionPauseDeathResetAndDimensions)
{
    auto asset = CreateDefaultSystem().emitters[0]; asset.spawn.nodes.clear(); asset.spawnRate = 0; asset.bursts = {{0,1}};
    asset.interpolatedSpawning = false;
    asset.attributes[0].defaultValue = {2,3,-4,0}; asset.attributes[1].defaultValue = {20,-30,40,0};
    asset.attributes[3].type = ValueType::Float2; asset.attributes[3].defaultValue = {-6,8,0,0};
    asset.attributes.back().defaultValue = {0.3f,0,0,0};
    auto emitter = MakeEmitter(asset);
    EXPECT_TRUE(emitter.Bounds().Empty());
    ASSERT_TRUE(emitter.Tick(1.0/60));
    EXPECT_EQ(emitter.Bounds().minimum,emitter.Data().Read(0,0)); EXPECT_EQ(emitter.Bounds().maximum,emitter.Bounds().minimum);
    EXPECT_FLOAT_EQ(emitter.Bounds().spriteRadius,5); EXPECT_EQ(emitter.Bounds().particleCount,1u);
    const auto old = emitter.Bounds(); emitter.SetPaused(true); ASSERT_TRUE(emitter.Tick(0.1));
    EXPECT_EQ(emitter.Bounds().minimum,old.minimum); emitter.SetPaused(false);
    ASSERT_TRUE(emitter.Tick(0.1)); EXPECT_GT(emitter.Bounds().minimum[0],old.minimum[0]);
    ASSERT_TRUE(emitter.Tick(0.4)); EXPECT_TRUE(emitter.Bounds().Empty()); EXPECT_EQ(emitter.Bounds().particleCount,0u);
    emitter.Reset(); EXPECT_TRUE(emitter.Bounds().Empty()); ASSERT_TRUE(emitter.Tick(1.0/60));
    EXPECT_FALSE(emitter.Bounds().Empty()); emitter.Deactivate(true); EXPECT_TRUE(emitter.Bounds().Empty());

    ParticleDataSet data; data.Initialize(asset.attributes,4);
    ASSERT_TRUE(data.Spawn(1)); ASSERT_TRUE(data.Spawn(2));
    data.Write(0,0,{std::numeric_limits<float>::quiet_NaN(),0,0,0});
    auto bounds = CalculateParticleBounds(data);
    EXPECT_EQ(bounds.invalidCount,1u); EXPECT_EQ(bounds.particleCount,2u); EXPECT_EQ(bounds.minimum,asset.attributes[0].defaultValue);
    data.Write(3,1,{std::numeric_limits<float>::infinity(),0,0,0});
    EXPECT_TRUE(CalculateParticleBounds(data).Empty());
}

TEST(VfxRuntimeTests, SpriteVisibilityUsesSixClipPlanesExtentAndWorldTransform)
{
    auto attributes = CreateDefaultSystem().emitters[0].attributes;
    attributes[3].type = ValueType::Float2;
    ParticleDataSet data; data.Initialize(attributes,16);
    const Value positions[] = {{0,0,0.5f,0},{-3,0,0.5f,0},{3,0,0.5f,0},{0,-3,0.5f,0},{0,3,0.5f,0},
        {0,0,-2,0},{0,0,3,0},{1.25f,0,0.5f,0},{0,0,std::numeric_limits<float>::quiet_NaN(),0}};
    for (uint32_t index = 0; index < std::size(positions); ++index)
    { ASSERT_TRUE(data.Spawn(index)); data.Write(0,index,positions[index]); data.Write(3,index,{0.1f,0.1f,0,0}); }
    data.Write(3,7,{1,0.1f,0,0});
    SpriteView view;
    auto statistics = CalculateParticleVisibility(data,view);
    EXPECT_EQ(statistics.particleCount,9u); EXPECT_EQ(statistics.visibleCount,2u); EXPECT_EQ(statistics.culledCount,6u); EXPECT_EQ(statistics.invalidCount,1u);
    view.frustumCulling = false; EXPECT_EQ(CalculateParticleVisibility(data,view).visibleCount,8u);
    view.frustumCulling = true; view.localToWorld[3] = 20;
    EXPECT_EQ(CalculateParticleVisibility(data,view).visibleCount,0u);

    data.Clear(); ASSERT_TRUE(data.Spawn(42)); data.Write(0,0,{10,20,30,0}); data.Write(3,0,{-6,8,0,0});
    const auto bounds = CalculateParticleBounds(data);
    const std::array<float,16> transform{0,-2,0,5,-3,0,0,6,0,0,0.1f,7,0,0,0,1};
    const auto world = TransformParticleBounds(bounds,transform,{1,0,0,0},{0,1,0,0});
    EXPECT_FLOAT_EQ(world.minimum[0],-40); EXPECT_FLOAT_EQ(world.maximum[0],-30);
    EXPECT_FLOAT_EQ(world.minimum[1],-29); EXPECT_FLOAT_EQ(world.maximum[1],-19);
    EXPECT_FLOAT_EQ(world.minimum[2],10); EXPECT_FLOAT_EQ(world.maximum[2],10);
    // Sprite dimensions remain in world units when the center's local space shrinks.
    view.localToWorld = {0.01f,0,0,0,0,0.01f,0,0,0,0,0.01f,0,0,0,0,1};
    EXPECT_EQ(CalculateParticleVisibility(data,view).visibleCount,1u);
}

TEST(VfxRuntimeTests, CompiledArtifactRunsWithoutSourceGraphsAndRejectsInvalidBytecode)
{
    const auto asset = CreateDefaultSystem();
    CompiledSystem compiled;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(asset,compiled,diagnostics));
    const auto bytes = SerializeCompiledSystem(compiled);
    CompiledSystem restored;
    std::string error;
    ASSERT_TRUE(DeserializeCompiledSystem(bytes,restored,error)) << error;
    EXPECT_TRUE(restored.asset.emitters[0].update.nodes.empty());
    CpuEmitter emitter;
    ASSERT_TRUE(emitter.Initialize(restored.asset.emitters[0],restored.emitters[0].spawn,restored.emitters[0].update,error));
    ASSERT_TRUE(emitter.Tick(0.1));
    EXPECT_EQ(emitter.Data().Count(),10u);
    auto data = nlohmann::json::parse(bytes);
    data["emitters"][0]["update"]["instructions"][3]["inputs"][0] = 999;
    EXPECT_FALSE(DeserializeCompiledSystem(data.dump(),restored,error));
}

TEST(VfxRuntimeTests, SystemCompilationRejectsCpuEmittersWithoutLifecycleAttributes)
{
    auto asset = CreateDefaultSystem();
    asset.emitters[0].attributes.pop_back();
    CompiledSystem output;
    std::vector<Diagnostic> diagnostics;
    EXPECT_FALSE(CompileSystem(asset,output,diagnostics));
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_NE(diagnostics[0].message.find("Lifetime"),std::string::npos);
}

TEST(VfxRuntimeTests, SaveReplacesAnExistingAsset)
{
    const auto path = std::filesystem::temp_directory_path() / (NLS::Guid::New().ToString() + ".vfx");
    auto asset = CreateDefaultSystem();
    std::string error;
    ASSERT_TRUE(SaveSystem(path,asset,error)) << error;
    asset.name = "Changed";
    ASSERT_TRUE(SaveSystem(path,asset,error)) << error;
    SystemAsset loaded;
    ASSERT_TRUE(LoadSystem(path,loaded,error)) << error;
    EXPECT_EQ(loaded.name,"Changed");
    std::filesystem::remove(path);
}

TEST(VfxRuntimeTests, RejectsCyclesMissingInputsAndDuplicateWriters)
{
    auto asset = CreateDefaultSystem();
    auto& graph = asset.emitters[0].update;
    EXPECT_FALSE(CanConnect(graph,5,4,0));
    graph.nodes[3].inputs[0] = 5;
    EXPECT_FALSE(CompileGraph(graph,asset.emitters[0].attributes).Succeeded());
    graph.nodes[3].inputs[0] = 99999;
    EXPECT_FALSE(CompileGraph(graph,asset.emitters[0].attributes).Succeeded());
    graph = CreateDefaultSystem().emitters[0].update;
    auto node = graph.nodes.back();
    node.id = graph.nextNodeId++;
    graph.nodes.push_back(node);
    EXPECT_FALSE(CompileGraph(graph,asset.emitters[0].attributes).Succeeded());
}

TEST(VfxRuntimeTests, CompilesVectorScalarBroadcastAndGeneratesGuardedComputeShader)
{
    const auto asset = CreateDefaultSystem();
    const auto compiled = CompileGraph(asset.emitters[0].update,asset.emitters[0].attributes);
    ASSERT_TRUE(compiled.Succeeded());
    EXPECT_NE(compiled.compiled.hlsl.find("if (index >= VfxCount) return;"),std::string::npos);
    EXPECT_NE(compiled.compiled.hlsl.find("[numthreads(64,1,1)]"),std::string::npos);
    auto emitter = MakeEmitter(asset.emitters[0]);
    ASSERT_TRUE(emitter.Tick(0.1));
    ASSERT_EQ(emitter.Data().Count(),10u);
    const auto age = emitter.Data().FindAttribute("Particles.Age");
    const auto position = emitter.Data().FindAttribute("Particles.Position");
    EXPECT_GT(emitter.Data().Read(age,0)[0],0.08f);
    EXPECT_NEAR(emitter.Data().Read(position,0)[1],
        (RandomFloat(1,1,RandomHash(1)^RandomHash(0),0,1)*2-1)*0.5f+3*emitter.Data().Read(age,0)[0],1e-5f);
}

TEST(VfxRuntimeTests, SeedResetAndSeekReproduceParticleState)
{
    const auto asset = CreateDefaultSystem();
    auto emitter = MakeEmitter(asset.emitters[0]);
    ASSERT_TRUE(emitter.Tick(0.5));
    const auto count = emitter.Data().Count();
    const auto position = emitter.Data().Read(0,0);
    ASSERT_TRUE(emitter.Seek(0.5));
    EXPECT_EQ(emitter.Data().Count(),count);
    EXPECT_EQ(emitter.Data().Read(0,0),position);
    emitter.SetPaused(true);
    EXPECT_TRUE(emitter.Tick(0.1));
    EXPECT_DOUBLE_EQ(emitter.Age(),0.5);
    EXPECT_FALSE(emitter.Tick(std::numeric_limits<double>::infinity()));
    EXPECT_FALSE(emitter.Tick(-1));
}

TEST(VfxRuntimeTests, CapacityDropsBirthsWithoutDeferringThemAndStopDrainsLiveParticles)
{
    auto asset = CreateDefaultSystem().emitters[0];
    asset.capacity = 3;
    auto emitter = MakeEmitter(asset);
    ASSERT_TRUE(emitter.Tick(0.1));
    EXPECT_EQ(emitter.Data().Count(),3u);
    EXPECT_EQ(emitter.Statistics().dropped,7u);
    emitter.Deactivate();
    ASSERT_TRUE(emitter.Tick(1));
    ASSERT_TRUE(emitter.Tick(1));
    EXPECT_TRUE(emitter.IsComplete());
    EXPECT_EQ(emitter.Statistics().spawned,3u);
    EXPECT_EQ(emitter.Statistics().died,3u);
}

TEST(VfxRuntimeTests, BurstsFireAtStartAndEachLoopBoundary)
{
    auto asset = CreateDefaultSystem().emitters[0];
    asset.spawnRate = 0;
    asset.duration = 0.1;
    asset.bursts = {{0,2},{0.05,1}};
    auto emitter = MakeEmitter(asset);
    ASSERT_TRUE(emitter.Tick(0.1));
    EXPECT_EQ(emitter.Statistics().spawned,5u);
    ASSERT_TRUE(emitter.Tick(0.1));
    EXPECT_EQ(emitter.Statistics().spawned,8u);
}

TEST(VfxRuntimeTests, MultipleBurstLoopsInsideOneStepKeepTheirActualBirthAges)
{
    auto asset = CreateDefaultSystem().emitters[0];
    asset.spawnRate = 0;
    asset.duration = 0.001;
    asset.bursts = {{0,1}};
    auto emitter = MakeEmitter(asset);
    ASSERT_TRUE(emitter.Tick(0.01));
    ASSERT_EQ(emitter.Data().Count(),11u);
    const auto age = emitter.Data().FindAttribute("Particles.Age");
    EXPECT_NEAR(emitter.Data().Read(age,0)[0],0.01f,1e-6f);
    EXPECT_NEAR(emitter.Data().Read(age,10)[0],0,1e-6f);
}

TEST(VfxRuntimeTests, CompactionPreservesIdentityAndAllAttributeColumns)
{
    ParticleDataSet data;
    data.Initialize({{"A",ValueType::Float3,{1,2,3,0}}},10);
    ASSERT_TRUE(data.Spawn(11)); ASSERT_TRUE(data.Spawn(22)); ASSERT_TRUE(data.Spawn(33));
    data.Write(0,2,{7,8,9,0});
    const std::array<uint8_t,3> mask{1,0,1};
    data.Compact(mask);
    ASSERT_EQ(data.Count(),2u);
    EXPECT_EQ(data.Id(1),33u);
    EXPECT_EQ(data.Read(0,1),(Value{7,8,9,0}));
}

TEST(VfxRuntimeTests, EmissionScheduleOrdersBirthsAndReservesIdsForCapacityDrops)
{
    auto asset = CreateDefaultSystem().emitters[0];
    asset.capacity = 6; asset.spawnRate = 1000; asset.duration = 0.1;
    asset.bursts = {{0,2},{0.005,2}};
    EmissionScheduler schedule;
    std::string error;
    ASSERT_TRUE(schedule.Initialize(asset,error)) << error;
    EmissionStep step;
    ASSERT_TRUE(schedule.Advance(0.01,step));
    EXPECT_EQ(step.requested,14u);
    std::vector<std::pair<uint64_t,float>> births;
    for (const auto& batch : step.batches)
        for (uint32_t index = 0; index < batch.count; ++index)
            births.emplace_back(batch.firstId+index,batch.initialAge-index*batch.ageStep);
    ASSERT_EQ(births.size(),6u);
    EXPECT_EQ(births[0].first,1u); EXPECT_NEAR(births[0].second,0.01f,1e-7f);
    EXPECT_EQ(births[1].first,2u); EXPECT_NEAR(births[1].second,0.01f,1e-7f);
    EXPECT_EQ(births[5].first,6u); EXPECT_NEAR(births[5].second,0.006f,1e-7f);
    ASSERT_TRUE(schedule.Advance(0.01,step));
    ASSERT_FALSE(step.batches.empty());
    EXPECT_EQ(step.batches[0].firstId,15u);
    const auto age = schedule.Age();
    EXPECT_FALSE(schedule.Advance(std::numeric_limits<double>::infinity(),step));
    EXPECT_EQ(schedule.Age(),age);
    schedule.Reset();
    asset.spawnRate = 10000000; asset.capacity = 1000000; asset.bursts.clear();
    ASSERT_TRUE(schedule.Initialize(asset,error));
    ASSERT_TRUE(schedule.Advance(1.0/60,step));
    EXPECT_EQ(step.requested,166666u);
    ASSERT_EQ(step.batches.size(),1u);
    EXPECT_EQ(step.batches[0].count,166666u);
}

TEST(VfxRuntimeTests, GpuCompilationRejectsInvalidTimingAndUnavailableCollision)
{
    auto asset = CreateDefaultSystem();
    auto& emitter = asset.emitters[0];
    emitter.target = SimulationTarget::GPU;
    CompiledSystem compiled;
    std::vector<Diagnostic> diagnostics;
    emitter.spawnRate = -1;
    EXPECT_FALSE(CompileSystem(asset,compiled,diagnostics));
    emitter.spawnRate = 100; emitter.collisionEnabled = true;
    EXPECT_FALSE(CompileSystem(asset,compiled,diagnostics));
    emitter.collisionEnabled = false;
    EXPECT_TRUE(CompileSystem(asset,compiled,diagnostics));
}

TEST(VfxRuntimeTests, UntrustedProgramCannotReadAFutureRegister)
{
    ParticleDataSet data;
    data.Initialize({},1);
    ASSERT_TRUE(data.Spawn(1));
    Program program;
    Instruction instruction;
    instruction.operation = Operation::Add;
    instruction.inputs = {1,1,0};
    program.instructions.push_back(instruction);
    std::vector<Value> registers;
    EXPECT_FALSE(RunParticleProgram(program, data, 0, 1, {}, registers));
}

TEST(VfxRuntimeTests, CpuCollisionUsesPhysXAndReflectsVelocity)
{
    auto asset = CreateDefaultSystem().emitters[0];
    asset.spawn.nodes.clear();
    asset.spawnRate = 0;
    asset.bursts = {{0,1}};
    asset.collisionEnabled = true;
    asset.collisionRadius = 0.05f;
    asset.attributes[0].defaultValue = {0,1,0,0};
    asset.attributes[1].defaultValue = {0,-10,0,0};
    auto emitter = MakeEmitter(asset);
    std::string error;
    auto world = NLS::Physics::PhysicsWorld::Create(error);
    ASSERT_NE(world,nullptr) << error;
    NLS::Physics::BodySettings settings;
    settings.position = {0,-0.5f,0};
    ASSERT_TRUE(world->AddBox({5,0.5f,5},settings).IsValid());
    emitter.SetCollisionWorld(world);
    ASSERT_TRUE(emitter.Tick(0.2));
    ASSERT_EQ(emitter.Data().Count(),1u);
    EXPECT_GT(emitter.Data().Read(0,0)[1],0);
    EXPECT_GT(emitter.Data().Read(1,0)[1],0);
    EXPECT_GT(emitter.Statistics().collisions,0u);
}

TEST(VfxRuntimeTests, CollisionResultsReachTheParticleColumnsAndClearOnMiss)
{
    const auto makeCollisionAsset = []
    {
        auto asset = CreateDefaultSystem().emitters[0];
        asset.spawn.nodes.clear();
        asset.spawnRate = 0;
        asset.bursts = {{0,1}};
        asset.collisionEnabled = true;
        asset.collisionRadius = 0.05f;
        asset.attributes[0].defaultValue = {0,1,0,0};
        asset.attributes[1].defaultValue = {0,-10,0,0};
        for (auto& attribute : asset.attributes) if (attribute.name == "Particles.Lifetime") attribute.defaultValue = {10,0,0,0};
        // Declaring the columns is what a Collision module does at authoring
        // time; the runtime only writes them when they exist.
        asset.attributes.push_back({"Particles.CollisionNormal",ValueType::Float3,{}});
        asset.attributes.push_back({"Particles.Colliding",ValueType::Float,{}});
        return asset;
    };

    auto asset = makeCollisionAsset();
    auto emitter = MakeEmitter(asset);
    std::string error;
    auto world = NLS::Physics::PhysicsWorld::Create(error);
    ASSERT_NE(world,nullptr) << error;
    NLS::Physics::BodySettings settings;
    settings.position = {0,-0.5f,0};
    ASSERT_TRUE(world->AddBox({5,0.5f,5},settings).IsValid());
    emitter.SetCollisionWorld(world);
    // One exact sub-step spanning the floor contact: the particle moves from
    // y=1 to y=0 with a 0.05 sphere, so the sweep reports the top face.
    ASSERT_TRUE(emitter.TickStep(0.1));
    ASSERT_EQ(emitter.Data().Count(),1u);
    const auto normal = emitter.Data().FindAttribute("Particles.CollisionNormal");
    const auto colliding = emitter.Data().FindAttribute("Particles.Colliding");
    ASSERT_GE(normal,0); ASSERT_GE(colliding,0);
    const auto normalValue = emitter.Data().Read(normal,0);
    // The floor's normal points up, so contact must publish +Y and flag 1.
    EXPECT_NEAR(normalValue[1],1.0f,1e-3f) << "the contact normal must reach the column";
    EXPECT_NEAR(emitter.Data().Read(colliding,0)[0],1.0f,1e-5f)
        << "a contact must flag the colliding column";

    // After the bounce the particle leaves the surface, so the next step has
    // no contact and both columns must be cleared rather than keeping a stale
    // normal.
    ASSERT_TRUE(emitter.TickStep(0.1));
    EXPECT_NEAR(emitter.Data().Read(colliding,0)[0],0.0f,1e-5f)
        << "losing contact must clear the flag";
    const auto clearedNormal = emitter.Data().Read(normal,0);
    EXPECT_NEAR(clearedNormal[0],0.0f,1e-5f);
    EXPECT_NEAR(clearedNormal[1],0.0f,1e-5f);
    EXPECT_NEAR(clearedNormal[2],0.0f,1e-5f);
    // A particle with upward motion far from the floor must report no contact.
    auto missAsset = makeCollisionAsset();
    // Give the particle upward motion far from the floor so nothing is hit.
    missAsset.attributes[0].defaultValue = {0,50,0,0};
    missAsset.attributes[1].defaultValue = {0,10,0,0};
    auto missEmitter = MakeEmitter(missAsset);
    auto missWorld = NLS::Physics::PhysicsWorld::Create(error);
    ASSERT_NE(missWorld,nullptr) << error;
    missEmitter.SetCollisionWorld(missWorld);
    ASSERT_TRUE(missEmitter.Tick(0.1));
    ASSERT_EQ(missEmitter.Data().Count(),1u);
    EXPECT_NEAR(missEmitter.Data().Read(missEmitter.Data().FindAttribute("Particles.Colliding"),0)[0],0.0f,1e-5f);
    const auto missNormal = missEmitter.Data().Read(missEmitter.Data().FindAttribute("Particles.CollisionNormal"),0);
    EXPECT_NEAR(missNormal[0],0.0f,1e-5f);
    EXPECT_NEAR(missNormal[1],0.0f,1e-5f);
    EXPECT_NEAR(missNormal[2],0.0f,1e-5f);
}

TEST(VfxRuntimeTests, EventGenerationMaskSuppressesDeathEventsAtThePublishSite)
{
    auto asset = CreateDefaultSystem().emitters[0];
    asset.spawn.nodes.clear();
    asset.spawnRate = 0;
    asset.bursts = {{0,4}};
    asset.attributes[1].defaultValue = {0,0,0,0};
    if (const auto lifetime = std::find_if(asset.attributes.begin(),asset.attributes.end(),
        [](const Attribute& attribute) { return attribute.name == "Particles.Lifetime"; }); lifetime != asset.attributes.end())
        lifetime->defaultValue = {0.25f,0,0,0};
    asset.eventGenerationMask = 0;
    auto emitter = MakeEmitter(asset);
    ASSERT_TRUE(emitter.Tick(0.1));
    ASSERT_GT(emitter.Data().Count(),0u);
    // Suppressed death events must not appear even though particles die.
    ASSERT_TRUE(emitter.Tick(1.0));
    EXPECT_GT(emitter.Statistics().died,0u);
    EXPECT_TRUE(std::none_of(emitter.Events().begin(),emitter.Events().end(),
        [](const ParticleEvent& event) { return event.kind == ParticleEventKind::Death; }));


    // Re-enabling the death channel restores the events at the publish site.
    auto publishingAsset = CreateDefaultSystem().emitters[0];
    publishingAsset.spawn.nodes.clear();
    publishingAsset.spawnRate = 0;
    publishingAsset.bursts = {{0,4}};
    publishingAsset.attributes[1].defaultValue = {0,0,0,0};
    for (auto& attribute : publishingAsset.attributes) if (attribute.name == "Particles.Lifetime") attribute.defaultValue = {0.25f,0,0,0};
    publishingAsset.eventGenerationMask = EventBit(ParticleEventSelection::Death);
    auto publishing = MakeEmitter(publishingAsset);
    ASSERT_TRUE(publishing.Tick(0.1)); ASSERT_TRUE(publishing.Tick(1.0));
    EXPECT_GT(publishing.Statistics().died,0u);
    EXPECT_FALSE(publishing.Events().empty());
    EXPECT_TRUE(std::all_of(publishing.Events().begin(),publishing.Events().end(),
        [](const ParticleEvent& event) { return event.kind == ParticleEventKind::Death; }));
}

TEST(VfxRuntimeTests, ParallelParticleUpdatesMatchTheSerialPathParticleForParticle)
{
    constexpr uint32_t particles = 3000u;
    const auto asset = MakeParallelBurstAsset(particles);
    auto serial = MakeEmitter(asset);
    auto parallel = MakeEmitter(asset);
    parallel.SetParallelExecutionEnabled(true);
    const ScopedVfxJobSystem jobSystem((std::max)(2u,std::thread::hardware_concurrency() / 2u));
    ASSERT_TRUE(jobSystem.Initialized()) << "Parallel coverage needs a live Jobs worker pool";
    ASSERT_TRUE(NLS::Base::Jobs::GetJobWorkerCount() >= 2u);

    serial.SetParallelExecutionEnabled(false);
    EXPECT_TRUE(serial.Tick(0.05));
    ASSERT_EQ(serial.Data().Count(),particles);
    EXPECT_EQ(serial.LastParallelTaskCount(),1u) << "Reference run must not fan out";

    ASSERT_TRUE(parallel.Tick(0.05));
    ASSERT_EQ(parallel.Data().Count(),particles);
    EXPECT_GT(parallel.LastParallelTaskCount(),1u) << "Parallel run must fan out";
    // Simulation work must stay on the general Jobs pool instead of the
    // rendering TaskGraph so it never contends with render lanes.
    EXPECT_EQ(parallel.LastParallelScheduler(),ParallelScheduler::Jobs)
        << "A live Jobs pool is the only supported parallel backend";

    // Every attribute column, id, statistic and event must be identical: the
    // fan-out only splits the per-particle inner loop and derives randomness
    // from the stable particle id.
    ExpectSameEmitterState(serial,parallel);
}

TEST(VfxRuntimeTests, ParallelParticleUpdatesMatchTheSerialPathAcrossManyFrames)
{
    constexpr uint32_t particles = 2600u;
    auto asset = MakeParallelBurstAsset(particles);
    // The first burst dies as one generation, the second one refills the freed
    // capacity, so the comparison covers the serial spawn/death stages that sit
    // between the fanned-out frames instead of a single steady-state step.
    for (auto& attribute : asset.attributes)
        if (attribute.name == "Particles.Lifetime") attribute.defaultValue = {0.2f,0,0,0};
    asset.bursts = {{0,particles},{0.25,particles}};
    auto serial = MakeEmitter(asset);
    auto parallel = MakeEmitter(asset);
    serial.SetParallelExecutionEnabled(false);
    parallel.SetParallelExecutionEnabled(true);

    const ScopedVfxJobSystem jobSystem((std::max)(2u,std::thread::hardware_concurrency() / 2u));
    ASSERT_TRUE(jobSystem.Initialized()) << "Parallel coverage needs a live Jobs worker pool";

    uint32_t fannedOutFrames = 0u;
    for (uint32_t frame = 0; frame < 30u; ++frame)
    {
        ASSERT_TRUE(serial.Tick(1.0 / 60.0));
        ASSERT_TRUE(parallel.Tick(1.0 / 60.0));
        if (parallel.LastParallelTaskCount() > 1u) ++fannedOutFrames;
    }
    EXPECT_GT(fannedOutFrames,10u);
    EXPECT_GT(serial.Statistics().died,particles);
    ExpectSameEmitterState(serial,parallel);
}

TEST(VfxRuntimeTests, ParallelParticleUpdateSwitchRoundTripsWithoutChangingTheResult)
{
    constexpr uint32_t particles = 3000u;
    const auto asset = MakeParallelBurstAsset(particles);
    auto reference = MakeEmitter(asset);
    auto emitter = MakeEmitter(asset);
    reference.SetParallelExecutionEnabled(false);

    // The switch is an A/B diagnostic, so flipping it mid-run must leave the
    // simulation identical to an all-serial run while reporting the path each
    // tick actually took.
    emitter.SetParallelExecutionEnabled(false);
    ASSERT_TRUE(emitter.Tick(1.0 / 60.0));
    EXPECT_EQ(emitter.LastParallelTaskCount(),1u);
    ASSERT_TRUE(reference.Tick(1.0 / 60.0));
    emitter.SetParallelExecutionEnabled(true);
    for (uint32_t frame = 0; frame < 3u; ++frame)
    {
        ASSERT_TRUE(emitter.Tick(1.0 / 60.0));
        ASSERT_TRUE(reference.Tick(1.0 / 60.0));
    }

    // Other suites may have started a Jobs pool, so the expectation follows
    // whichever pool this process currently has.
    const bool fanOutAvailable =
        NLS::Base::Jobs::IsJobSystemInitialized() && NLS::Base::Jobs::GetJobWorkerCount() > 0u;
    EXPECT_EQ(emitter.LastParallelTaskCount() > 1u,fanOutAvailable)
        << "Fan-out must be reported exactly when a scheduler can serve it";
    EXPECT_EQ(emitter.LastParallelScheduler(),
        fanOutAvailable ? ParallelScheduler::Jobs : ParallelScheduler::Serial);
    ExpectSameEmitterState(reference,emitter);
}
