#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <span>
#include <unordered_map>
#include <vector>

#include "VfxDef.h"

namespace NLS::VFX
{
using Value = std::array<float, 4>;
using NodeId = uint64_t;

enum class ValueType : uint8_t { Float, Float2, Float3, Float4, Int32, Bool };
inline constexpr bool IsFloatType(ValueType type) { return type <= ValueType::Float4; }
inline constexpr bool IsIntegralType(ValueType type) { return type == ValueType::Int32 || type == ValueType::Bool; }
inline constexpr bool IsValueType(ValueType type) { return type <= ValueType::Bool; }
inline Value IntValue(int32_t value) { return {std::bit_cast<float>(value),0,0,0}; }
inline Value BoolValue(bool value) { return IntValue(value ? -1 : 0); }
inline int32_t ReadInt(const Value& value) { return std::bit_cast<int32_t>(value[0]); }
inline bool ReadBool(const Value& value) { return ReadInt(value) != 0; }
inline bool ValidValue(const Value& value,ValueType type)
{
    if (!IsValueType(type)) return false;
    if (type == ValueType::Bool && ReadInt(value) != 0 && ReadInt(value) != -1) return false;
    for (size_t i = IsIntegralType(type) ? 1 : 0; i < value.size(); ++i) if (!std::isfinite(value[i])) return false;
    return true;
}
inline bool SameValue(const Value& a,const Value& b)
{ return std::bit_cast<std::array<uint32_t,4>>(a) == std::bit_cast<std::array<uint32_t,4>>(b); }
enum class ScriptStage : uint8_t { ParticleSpawn, ParticleUpdate, SimulationStage, EmitterSpawn, EmitterUpdate, Event, SystemSpawn, SystemUpdate };
enum class SimulationTarget : uint8_t { CPU, GPU };
enum class RendererType : uint8_t { Sprite, Mesh, Ribbon };
// Which published particle events an emitter's event handler consumes. This
// mirrors the Niagara Generate/Receive pairing without introducing a separate
// execution stage: the receiving emitter spawns from the matching events.
enum class ParticleEventSelection : uint8_t { None, Spawn, Death, Collision };
// Which receiver particles an Event Handler applies its event script/payload to.
enum class EventExecutionMode : uint8_t { SpawnedParticles, EveryParticle, SingleParticle };
enum class ParticleEventGeneration : uint8_t { Spawn = 1u << 0, Death = 1u << 1, Collision = 1u << 2 };
inline constexpr uint32_t kParticleEventGenerationAll = 0x7u;
inline uint32_t EventBit(ParticleEventSelection selection)
{
    switch (selection) { case ParticleEventSelection::Spawn: return 1u << 0; case ParticleEventSelection::Death: return 1u << 1; case ParticleEventSelection::Collision: return 1u << 2; default: return 0u; }
}
enum class Operation : uint8_t
{
    Constant, Attribute, Parameter, DeltaTime, NormalizedAge, Random,
    Add, Subtract, Multiply, Divide, Minimum, Maximum, Clamp, Lerp,
    Sine, Cosine, Absolute, SquareRoot, Normalize, Length, Dot, Cross,
    WriteAttribute, StageIndex, StageIteration,
    Equal, NotEqual, LessThan, LessEqual, GreaterThan, GreaterEqual, LogicalAnd, LogicalOr, LogicalNot, Select, Convert,
    // Appended at the end so existing opcode numbering and serialized programs
    // stay valid. SpawnIndex exposes the particle's birth ordinal within the
    // current spawn batch, which grid/partition modules need.
    SpawnIndex, StructMake, StructGet, DataInterfaceCall
};

struct Node
{
    NodeId id = 0;
    Operation operation = Operation::Constant;
    ValueType type = ValueType::Float;
    Value value {};
    std::string name;
    std::vector<NodeId> inputs;
    std::array<float, 2> position {};
    // Optional aggregate type for Parameter/Attribute/WriteAttribute nodes.
    std::string structType;
};

struct StructFieldDefinition
{
    std::string name;
    ValueType type = ValueType::Float;
    std::string structType;
    Value defaultValue{};
    bool operator==(const StructFieldDefinition& other) const
    { return name==other.name && type==other.type && structType==other.structType && SameValue(defaultValue,other.defaultValue); }
};
struct StructDefinition
{
    std::string name;
    std::vector<StructFieldDefinition> fields;
    bool operator==(const StructDefinition&) const = default;
};
enum class DataInterfaceKind : uint8_t { Array, Curve };
struct DataInterfaceDefinition
{
    std::string name;
    DataInterfaceKind kind = DataInterfaceKind::Array;
    ValueType elementType = ValueType::Float;
    // Array elements, or Curve (time,value,0,0) keys. Binding size is fixed by
    // compilation; instances may replace values without recompiling shaders.
    std::vector<Value> data;
    bool operator==(const DataInterfaceDefinition& other) const
    {
        if(name!=other.name || kind!=other.kind || elementType!=other.elementType || data.size()!=other.data.size())return false;
        for(size_t i=0;i<data.size();++i)if(!SameValue(data[i],other.data[i]))return false;
        return true;
    }
};

struct Graph
{
    std::string id;
    ScriptStage stage = ScriptStage::ParticleUpdate;
    std::vector<Node> nodes;
    NodeId nextNodeId = 1;
    std::vector<StructDefinition> structs;
    std::vector<DataInterfaceDefinition> dataInterfaces;
};

struct Attribute
{
    std::string name;
    ValueType type = ValueType::Float;
    Value defaultValue {};
};

struct Burst
{
    double time = 0;
    uint32_t count = 0;
};
// CPU-published event payload queued for a GPU receiver. The render proxy
// forwards these to GpuEmitter before its next frame recording. The receiver's
// handler script runs on the CPU, so the payload already carries the resolved
// spawn count, random age and the birth position/velocity overrides.
struct GpuEventSpawn
{
    Value position {};
    Value velocity {};
    Value normal {};
    uint32_t count = 1;
    float randomAge = 0;
    float kind = 0;
    float particleId = 0;
    float sourceIndex = 0;
    bool overrideVelocity = false;
    uint32_t handlerIndex = 0;
    EventExecutionMode executionMode = EventExecutionMode::SpawnedParticles;
    uint64_t targetParticleId = 0;
};

struct SimulationStageBindings
{
    std::string enabled, iterations;
    bool enabledDefault = true; // Runtime fallback, baked from the System parameter.
    bool typed = false; // v4 bindings use Bool/Int32; v3 bindings retain scalar compatibility.
};
struct SimulationStageExecution
{
    bool enabled = false;
    uint32_t iterations = 0;
};
struct SimulationStage
{
    std::string id;
    std::string name = "Simulation Stage";
    bool enabled = true;
    uint32_t iterations = 1;
    Graph graph;
    SimulationStageBindings bindings;
};

struct AssetReference
{
    std::string guid;
    int64_t localId = 0;
    [[nodiscard]] bool Empty() const { return guid.empty() && localId == 0; }
};
enum class SpriteBlendMode : uint8_t { Translucent, Additive, Masked, Opaque, Premultiplied };
enum class SpriteAlignment : uint8_t { Camera, Velocity };
struct DynamicMaterialBinding
{
    std::string attribute;
    std::string fallbackParameter;
    Value defaultValue{1,1,1,1};
};
struct MaterialParameterBinding
{
    std::string materialParameter, systemParameter;
    ValueType type = ValueType::Float;
    Value defaultValue{};
};
struct SpriteSettings
{
    AssetReference material, texture;
    SpriteBlendMode blend = SpriteBlendMode::Translucent;
    SpriteAlignment alignment = SpriteAlignment::Camera;
    Value tint{1,1,1,1};
    float emissive = 1, alphaCutoff = 0.333f, softDistance = 0;
    uint32_t subImagesX = 1, subImagesY = 1;
    bool blendSubImages = false;
    bool circularMask = true;
    bool useMaterialShader = false;
    std::array<DynamicMaterialBinding,4> dynamicParameters{{
        {"Particles.DynamicMaterialParameter"},{"Particles.DynamicMaterialParameter1"},
        {"Particles.DynamicMaterialParameter2"},{"Particles.DynamicMaterialParameter3"}}};
    std::vector<MaterialParameterBinding> materialParameters;
};

// A mesh renderer draws one slot per particle. Particles.MeshIndex selects the
// slot; an empty slot list is invalid and the count is capped at 8.
inline constexpr std::size_t kMaxMeshSlots = 8;
struct MeshSettings
{
    std::vector<AssetReference> meshes;
    // Optional per-slot material override; an empty entry uses the emitter material.
    std::vector<AssetReference> materials;
    std::string meshIndexAttribute = "Particles.MeshIndex";
    // Negative selects a LOD from the projected screen size; otherwise the
    // clamped level index is used for every slot.
    int32_t lodIndex = -1;
    std::string scaleAttribute = "Particles.Scale";
    std::string orientationAttribute = "Particles.MeshOrientation";
    Value defaultScale{1,1,1,0};
    // Quaternion in xyzw order, normalized by the renderer. Zero selects identity.
    Value defaultOrientation{0,0,0,1};
    [[nodiscard]] std::size_t SlotCount() const { return meshes.size(); }
};
NLS_VFX_API bool ValidateMeshSettings(const MeshSettings& settings, std::string& error);
// Per-slot draw settings: a set override replaces the emitter material reference.
NLS_VFX_API SpriteSettings ResolveMeshSlotMaterialSettings(const SpriteSettings& sprite, const MeshSettings& mesh, std::size_t slot);

// Ribbon/beam renderer. The strip connects particles in draw order; the width
// comes from a particle attribute so a module can drive it over time.
struct RibbonSettings
{
    // Empty selects the default "Particles.RibbonWidth" column.
    std::string widthAttribute = "Particles.RibbonWidth";
    // Reuses the sprite material and texture slot so a ribbon can be textured
    // with the same asset pipeline as a sprite.
    AssetReference material, texture;
    SpriteBlendMode blend = SpriteBlendMode::Additive;
    Value tint{1,1,1,1};
    float emissive = 1;
    float uvTilingDistance = 1;
    bool tileUvOverDistance = false;
    bool useMaterialShader = false;
};
NLS_VFX_API bool ValidateRibbonSettings(const RibbonSettings& settings, std::string& error);
// Maps ribbon settings onto the shared particle draw settings so the existing
// renderer submission path (blend, tint, emissive, material) can be reused
// without a second settings concept.
NLS_VFX_API SpriteSettings ResolveRibbonDrawSettings(const RibbonSettings& settings);

struct EmitterAsset
{
    std::string id;
    std::string name = "Emitter";
    SimulationTarget target = SimulationTarget::CPU;
    uint32_t capacity = 10000;
    // Zero selects a small initial allocation; capacity remains the particle limit.
    uint32_t initialCapacity = 0;
    uint32_t seed = 1;
    double spawnRate = 100;
    // Spawn Per Frame: an exact per-sub-step particle count, independent of the
    // elapsed delta. Kept separate from spawnRate because the rate path scales
    // with time while this must stay frame based.
    uint32_t spawnPerFrame = 0;
    // Spawn Per Unit: particles per world unit the owning system travels. The
    // fractional remainder carries across steps so slow motion still emits.
    double spawnPerUnit = 0;
    double duration = 5;
    bool looping = true;
    bool interpolatedSpawning = true;
    bool sortByViewDepth = false;
    bool frustumCulling = true;
    bool collisionEnabled = false;
    float collisionRadius = 0;
    float restitution = 0.5f;
    float friction = 0;
    uint32_t collisionLayers = ~0u;
    // Event handler reception. When eventKind is set the emitter spawns
    // particles at the positions of the chosen events published by another
    // emitter in the same System instead of using its own emission schedule.
    struct EventReceiver
    {
        std::string sourceEmitter;          // Empty accepts any emitter in the System.
        ParticleEventSelection eventKind = ParticleEventSelection::None;
        uint32_t spawnCount = 1;            // Particles created per received event.
        float randomAge = 0;                // Randomises the initial age in [0, randomAge].
        // Maximum events consumed by this handler per System tick; zero means
        // unlimited. Mirrors Niagara's Max Events Per Frame handler property.
        uint32_t maxEventsPerFrame = 0;
        EventExecutionMode executionMode = EventExecutionMode::SpawnedParticles;
        bool Empty() const { return eventKind == ParticleEventSelection::None; }
    };
    struct EventHandler
    {
        std::string id;                     // Stable Event stack identity.
        std::string name;                   // Authoring/debug label.
        EventReceiver receiver;
        Graph graph;
    };
    // One entry per Event stack. Each handler has its own receiver policy and
    // per-event program, matching Niagara's independently executed handlers.
    std::vector<EventHandler> eventHandlers;
    // Generation is independent from reception. Historically every CPU
    // emitter published spawn/death/collision events; keeping all bits as the
    // default preserves that behavior for existing assets while Generate
    // modules can now suppress individual channels.
    uint32_t eventGenerationMask = kParticleEventGenerationAll;
    // Set once an authoring Generate module defines the policy. A materialized
    // emitter without any Generate module keeps publishing every channel so
    // existing assets retain their behavior.
    bool hasEventGenerationPolicy = false;
    std::vector<Burst> bursts;
    std::vector<Attribute> attributes;
    Graph spawn;
    Graph update;
    // Emitter-stage attribute schema and scripts. Emitter.* channels are
    // evaluated by the CPU scheduler for both simulation targets.
    std::vector<Attribute> emitterAttributes;
    Graph emitterSpawn;
    Graph emitterUpdate;
    Graph emitterCollision;
    std::vector<SimulationStage> simulationStages;
    SpriteSettings sprite;
    RendererType renderer = RendererType::Sprite;
    MeshSettings mesh;
    RibbonSettings ribbon;
};

struct ParameterDefinition
{
    std::string name;
    ValueType type = ValueType::Float;
    Value value{};
};
// System-level completion policy. When enabled the System reports complete once
// every emitter has held zero particles for the configured grace period, which
// lets externally driven systems stay alive while waiting for input.
struct SystemCompletionSettings
{
    bool enabled = false;
    double unusedGracePeriod = 0.5;
    bool operator==(const SystemCompletionSettings&) const = default;
};

struct SystemTimingSettings
{
    bool fixedTick = false;
    double fixedTickDelta = 0.01667;
    uint32_t warmupTickCount = 0;
    double warmupTickDelta = 1.0/15.0;
    double WarmupTime() const { return warmupTickCount*warmupTickDelta; }
    bool operator==(const SystemTimingSettings&) const = default;
};
inline constexpr uint32_t kMaxSystemFixedTicks = 100;
inline constexpr uint32_t kMaxSystemWarmupTicks = 1024;
NLS_VFX_API bool ValidateSystemTiming(const SystemTimingSettings& settings,std::string& error);
NLS_VFX_API bool SetSystemWarmupTime(SystemTimingSettings& settings,double seconds);
struct SystemAsset
{
    uint32_t version = 1;
    std::string id;
    std::string name = "Particle System";
    std::vector<EmitterAsset> emitters;
    std::vector<ParameterDefinition> parameters;
    // System-stage script state. SystemSpawn runs once per system activation;
    // SystemUpdate runs once per System tick before emitter updates.
    std::vector<Attribute> systemAttributes;
    Graph systemSpawn;
    Graph systemUpdate;
    SystemTimingSettings timing;
    SystemCompletionSettings completion;
};

struct Diagnostic
{
    std::string message;
    std::string graphId;
    NodeId node = 0;
};

NLS_VFX_API uint32_t ComponentCount(ValueType type);
NLS_VFX_API const char* OperationName(Operation operation);
NLS_VFX_API uint32_t InputCount(Operation operation);
inline bool ValidInputCount(Operation operation,size_t count)
{
    if (operation == Operation::StructMake) return count > 0 && count <= 64;
    if (operation == Operation::DataInterfaceCall) return count <= 1;
    return count == InputCount(operation);
}
NLS_VFX_API Node* FindNode(Graph& graph, NodeId id);
NLS_VFX_API const Node* FindNode(const Graph& graph, NodeId id);
NLS_VFX_API NodeId AddNode(Graph& graph, Operation operation, ValueType type = ValueType::Float);
NLS_VFX_API bool CanConnect(const Graph& graph, NodeId source, NodeId destination, uint32_t input);
NLS_VFX_API SystemAsset CreateDefaultSystem(std::string name = "Particle System");
NLS_VFX_API SimulationStage CreateSimulationStage(std::string name = "Simulation Stage");
NLS_VFX_API bool ValidateSimulationStageBindings(const SimulationStageBindings& bindings, std::string& error);
NLS_VFX_API bool ResolveSimulationStageControl(bool enabled, uint32_t iterations, const SimulationStageBindings& bindings,
    const std::unordered_map<std::string,Value>& parameters, SimulationStageExecution& execution, std::string& error);
NLS_VFX_API bool ResolveSimulationStageControls(std::span<const SimulationStage> stages,
    const std::unordered_map<std::string,Value>& parameters, std::vector<SimulationStageExecution>& execution, std::string& error);
NLS_VFX_API bool ValidateSpriteSettings(const SpriteSettings& settings, std::string& error);
NLS_VFX_API std::string SerializeSystem(const SystemAsset& asset, bool includeEditorData = true);
NLS_VFX_API bool DeserializeSystem(std::string_view text, SystemAsset& asset, std::string& error);
NLS_VFX_API bool LoadSystem(const std::filesystem::path& path, SystemAsset& asset, std::string& error);
NLS_VFX_API bool SaveSystem(const std::filesystem::path& path, const SystemAsset& asset, std::string& error);
}
