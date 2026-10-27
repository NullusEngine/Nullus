#pragma once

#include <map>
#include <optional>
#include <span>
#include <VFX/VfxGraph.h>

namespace NLS::VFX::Authoring
{
inline constexpr uint32_t kSchemaVersion = 2;
enum class AssetKind : uint8_t { System, Emitter, Module, Function, DynamicInput, ParameterCollection, ParameterDefinitions, EffectType, DataChannel, SimCache };
enum class Stage : uint8_t { SystemSpawn, SystemUpdate, EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, Event, Simulation };
enum class InputMode : uint8_t { Literal, Parameter, DynamicInput, Curve, Script };
enum class CurveInterpolation : uint8_t { Linear, Cubic };
struct VersionReference
{
    std::string asset, version;
    bool Empty() const { return asset.empty() && version.empty(); }
};
struct CurveKey
{
    float time = 0;
    Value value{}, arriveTangent{}, leaveTangent{};
    CurveInterpolation interpolation = CurveInterpolation::Linear;
};
NLS_VFX_API Value EvaluateCurve(std::span<const CurveKey> keys, float time);

struct Parameter
{
    std::string id, name;
    ValueType type = ValueType::Float;
    Value value{};
};
struct Input
{
    InputMode mode = InputMode::Literal;
    Value value{};
    std::string parameter;
    // A single WriteAttribute named "Output" is the expression result.
    Graph expression;
    std::vector<CurveKey> curve;
    VersionReference scriptAsset;
    std::map<std::string,Value> scriptArguments;
    ValueType type = ValueType::Float;
    std::map<std::string,ValueType> scriptArgumentTypes;
};
struct Module
{
    std::string id, script;
    bool enabled = true;
    std::map<std::string,Input> inputs;
    VersionReference scriptAsset;
    std::string inheritedId; // Filled only by SourceLibrary::Resolve.
};
struct Stack
{
    std::string id;
    Stage stage = Stage::ParticleUpdate;
    std::string name;
    bool enabled = true;
    uint32_t iterations = 1;
    std::vector<Module> modules;
    std::string inheritedId; // Filled only by SourceLibrary::Resolve.
    SimulationStageBindings bindings;
};
// Editor-only annotations persisted in the source asset. They never reach the
// runtime artifact and SemanticSource() omits them.
struct Comment
{
    std::string id, text;
    std::array<float,2> position{0,0};
    std::array<float,2> size{220,120};
    uint32_t color = 0xFF2A2A3A; // ImGui IM_COL32 layout (ABGR).
};
struct Group
{
    std::string id, title;
    std::vector<std::string> emitters;
};
struct Emitter
{
    EmitterAsset settings;
    bool enabled = true;
    std::array<float,2> position{};
    std::vector<Stack> stacks;
    VersionReference parent;
    // A referenced emitter inherits settings and modules. Local stacks append
    // modules; overrides address stable module IDs from the referenced version.
    struct ModuleOverride
    {
        std::string module;
        std::optional<bool> enabled;
        std::map<std::string,Input> inputs;
    };
    std::vector<ModuleOverride> overrides;
    std::vector<std::string> settingsOverrides;
    struct StackOverride
    {
        std::string stack;
        std::optional<bool> enabled;
        std::optional<uint32_t> iterations;
        std::optional<std::string> name;
        std::optional<SimulationStageBindings> bindings;
    };
    std::vector<StackOverride> stackOverrides;
};
struct System
{
    uint32_t version = kSchemaVersion;
    AssetKind kind = AssetKind::System;
    std::string id, name;
    std::vector<Parameter> parameters;
    // System-stage graph state, evaluated once (Spawn) and per tick (Update)
    // before the emitter passes.
    std::vector<Attribute> systemAttributes;
    Graph systemSpawn;
    Graph systemUpdate;
    std::vector<Emitter> emitters;
    std::vector<Comment> comments;
    std::vector<Group> groups;
    SystemTimingSettings timing;
    SystemCompletionSettings completion;
};

struct InputMetadata
{
    // Optional authoring metadata. ASCII names only; the editor falls back to
    // the input name when displayName is empty.
    std::string displayName;
    std::string tooltip;
    std::string unit;      // e.g. "cm", "cm/s", "deg", "s", "x"
    std::string visibleIf; // input name; the row is hidden while it is zero
    bool hasRange = false;
    float minimum = 0, maximum = 0, step = 0;
};
// Semantic capabilities consumed by the compiler when a module participates in
// stack-level policies (force ordering, collision, spawning and partitioning).
// They are authored on the module asset so neither the stack nor the compiler
// needs to know a code-defined module id.
enum ModuleCapability : uint32_t
{
    ModuleCapability_None = 0,
    ModuleCapability_ForceLimit = 1u << 0,
    ModuleCapability_ConsumesForce = 1u << 1,
    ModuleCapability_ConsumesDrag = 1u << 2,
    ModuleCapability_Collision = 1u << 3,
    ModuleCapability_SpawnBurst = 1u << 4,
    ModuleCapability_CompleteIfUnused = 1u << 5,
    ModuleCapability_PartitionParticles = 1u << 6,
    ModuleCapability_EngineValue = 1u << 7,
    ModuleCapability_Camera = 1u << 8,
    ModuleCapability_All = (1u << 9) - 1,
};
inline ModuleCapability operator|(ModuleCapability a, ModuleCapability b)
{ return static_cast<ModuleCapability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
inline uint32_t operator&(uint32_t flags, ModuleCapability capability)
{ return flags & static_cast<uint32_t>(capability); }
struct InputDefinition
{
    std::string name;
    ValueType type = ValueType::Float;
    Value defaultValue{};
    InputMetadata metadata;
};
struct ModuleDefinition
{
    std::string id, name, category, description;
    Stage stage;
    std::vector<InputDefinition> inputs;
    std::string requiresModule;
    std::vector<Stage> additionalStages;
    // Attributes the module reads or writes. The stack lowering registers any
    // missing ones on the emitter before the module graph is built, so a
    // renderer can consume them (for example Particles.Rotation for sprites).
    std::vector<InputDefinition> attributes;
    // Stack-level policy flags copied into the generated module asset.
    uint32_t capabilities = ModuleCapability_None;
};
struct Issue
{
    std::string message, emitter, stack, module, input;
};
class SourceLibrary;

NLS_VFX_API std::span<const ModuleDefinition> ModuleLibrary();
NLS_VFX_API const ModuleDefinition* FindModule(std::string_view id);
NLS_VFX_API bool SupportsStage(const ModuleDefinition& module, Stage stage);
NLS_VFX_API const char* StageName(Stage stage);
NLS_VFX_API System CreateSystem(std::string name = "Particle System");
NLS_VFX_API Emitter CreateEmitter(std::string name = "Emitter");
NLS_VFX_API Module CreateModule(std::string_view script);
NLS_VFX_API Stack* FindStack(Emitter& emitter, Stage stage);
NLS_VFX_API Stack CreateSimulationStage(std::string name = "Simulation Stage");
NLS_VFX_API bool MoveSimulationStage(Emitter& emitter, std::string_view stack, size_t index, std::string& error);
NLS_VFX_API std::string DuplicateSimulationStage(Emitter& emitter, std::string_view stack, std::string& error);
NLS_VFX_API bool Validate(const System& system, std::vector<Issue>& issues);
NLS_VFX_API std::string Serialize(const System& system, bool editorData = true);
NLS_VFX_API bool Deserialize(std::string_view text, System& system, std::string& error);
NLS_VFX_API bool Load(const std::filesystem::path& path, System& system, std::string& error);
NLS_VFX_API bool IsSystemSource(std::string_view text);
NLS_VFX_API bool Save(const std::filesystem::path& path, const System& system, std::string& error);
// Excludes graph positions and overview layout; semantic changes alter this key.
NLS_VFX_API std::string SemanticSource(const System& system);
NLS_VFX_API bool MoveModule(System& system, std::string_view module, std::string_view destinationStack, size_t index, std::string& error,const SourceLibrary* library = nullptr);
NLS_VFX_API std::string DuplicateModule(System& system, std::string_view module, std::string& error);
}
