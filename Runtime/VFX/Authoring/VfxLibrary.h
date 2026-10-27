#pragma once
#include "VfxAuthoring.h"
#include <functional>
#include <set>

namespace NLS::VFX::Authoring
{
struct ScriptInput
{
    std::string id, name;
    ValueType type = ValueType::Float;
    Value defaultValue{};
    bool isStatic = false;
    InputMetadata metadata;
};
// A call's result is represented by a Parameter node. The call metadata owns
// its named arguments; the node is eliminated before runtime IR compilation.
struct ScriptCall
{
    NodeId node = 0;
    VersionReference script;
    std::string output = "Output";
    std::map<std::string,NodeId> arguments;
};
struct ScriptSwitch
{
    NodeId node = 0, whenTrue = 0, whenFalse = 0;
    std::string input;
};
struct StageGraph
{
    Stage stage = Stage::ParticleUpdate;
    Graph graph;
};
struct ScriptVersion
{
    std::string id;
    uint32_t major = 1, minor = 0;
    std::string description;
    // Stack-level policy flags authored on the asset (force ordering, collision,
    // burst handling, partitioning). Compilers read these instead of module ids.
    uint32_t capabilities = ModuleCapability_None;
    std::vector<Stage> stages{Stage::ParticleSpawn,Stage::ParticleUpdate};
    std::vector<ScriptInput> inputs;
    // Engine module renderer/input attribute contract, copied from the
    // generator definition so runtime compilation does not need to look up the
    // code-defined module table.
    std::vector<Attribute> attributes;
    Graph graph;
    // Modules whose lowering depends on the execution stage provide one graph
    // per stage; a missing entry falls back to graph.
    std::vector<StageGraph> stageGraphs;
    std::vector<ScriptCall> calls;
    std::vector<ScriptSwitch> switches;
    std::vector<std::string> provides, requiresBefore;
};
struct ScriptAsset
{
    uint32_t version = kSchemaVersion;
    AssetKind kind = AssetKind::Module;
    std::string id, name, category, description, exposedVersion;
    bool visible = true, suggested = false, deprecated = false;
    std::vector<ScriptVersion> versions;
};
struct EmitterVersion
{
    std::string id;
    uint32_t major = 1, minor = 0;
    std::string description;
    Emitter emitter;
};
struct EmitterSource
{
    uint32_t version = kSchemaVersion;
    std::string id, name, exposedVersion;
    std::vector<EmitterVersion> versions;
};
struct SourceDependency
{
    std::string asset, semanticSource;
    std::filesystem::path path;
};

NLS_VFX_API ScriptAsset CreateScript(AssetKind kind, std::string name);
// Copies a script asset with fresh asset/version/input/graph identities. The
// captured graph and capability flags remain the complete module contract.
NLS_VFX_API ScriptAsset DuplicateScript(const ScriptAsset& source);
// True when path is inside root (both normalized); used to mark engine content
// as read-only.
NLS_VFX_API bool IsPathWithin(const std::filesystem::path& path, const std::filesystem::path& root);
// Engine-provided Module asset generated from a module definition. Ids are
// deterministic so regenerating engine content is stable across builds; the
// optional graph is the captured lowering and the version owns the full input,
// attribute and capability contract.
NLS_VFX_API ScriptAsset CreateEngineModuleAsset(const ModuleDefinition& definition, const Graph& graph = {});
NLS_VFX_API std::string EngineModuleAssetId(std::string_view moduleId);
NLS_VFX_API std::string EngineModuleVersionId(std::string_view moduleId);
NLS_VFX_API EmitterSource CreateEmitterSource(std::string name);
NLS_VFX_API const ScriptVersion* SelectVersion(const ScriptAsset& asset, const std::string& version);
NLS_VFX_API const EmitterVersion* SelectVersion(const EmitterSource& asset, const std::string& version);
NLS_VFX_API bool ValidateScript(const ScriptAsset& asset, std::string& error);
NLS_VFX_API bool ValidateEmitterSource(const EmitterSource& asset, std::string& error);
NLS_VFX_API std::string SerializeScript(const ScriptAsset& asset, bool editorData = true);
NLS_VFX_API bool DeserializeScript(std::string_view text, ScriptAsset& asset, std::string& error);
NLS_VFX_API std::string SerializeEmitterSource(const EmitterSource& asset, bool editorData = true);
NLS_VFX_API bool DeserializeEmitterSource(std::string_view text, EmitterSource& asset, std::string& error);
NLS_VFX_API bool SaveScript(const std::filesystem::path& path, const ScriptAsset& asset, std::string& error);
NLS_VFX_API bool SaveEmitterSource(const std::filesystem::path& path, const EmitterSource& asset, std::string& error);
NLS_VFX_API bool LoadScript(const std::filesystem::path& path, ScriptAsset& asset, std::string& error);
NLS_VFX_API bool LoadEmitterSource(const std::filesystem::path& path, EmitterSource& asset, std::string& error);
NLS_VFX_API Module InstantiateModule(const ScriptAsset& asset, const std::string& version = {});
NLS_VFX_API Emitter InstantiateEmitter(const EmitterSource& asset, const std::string& version = {});

// Immutable copies are captured by compile jobs. Refresh builds a candidate
// library and publishes only on success; ambiguous GUIDs never choose a file.
class NLS_VFX_API SourceLibrary final
{
public:
    bool Add(ScriptAsset asset, std::filesystem::path path, std::string& error);
    bool Add(EmitterSource asset, std::filesystem::path path, std::string& error);
    bool Refresh(std::span<const std::filesystem::path> roots, std::string& error);
    const ScriptAsset* Script(std::string_view id) const;
    const EmitterSource* EmitterAsset(std::string_view id) const;
    const std::map<std::string,ScriptAsset>& Scripts() const { return m_scripts; }
    const std::map<std::string,EmitterSource>& Emitters() const { return m_emitters; }
    bool Resolve(const System& source, System& result, std::vector<SourceDependency>& dependencies, std::vector<Issue>& issues) const;
    std::optional<SourceDependency> Dependency(std::string_view asset) const;
    std::string SemanticSource() const;
private:
    std::map<std::string,ScriptAsset> m_scripts;
    std::map<std::string,EmitterSource> m_emitters;
    std::map<std::string,std::filesystem::path> m_paths;
};
}
