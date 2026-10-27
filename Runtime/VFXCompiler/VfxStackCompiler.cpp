#include <VFX/VfxAggregateLowering.h>
#include "VfxStackCompiler.h"
#include <VFX/VfxTypeRules.h>
#include <VFX/VfxCompileCache.h>
#include <Guid.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace NLS::VFX::Authoring
{
namespace
{
const Graph& ScriptGraph(const ScriptVersion& version,Stage stage)
{
    for (const auto& staged : version.stageGraphs)
        if (staged.stage == stage) return staged.graph;
    return version.graph;
}
std::string ScopedGraphId(std::string_view key)
{
    const auto hash = [](std::string_view text,uint64_t seed) { for (unsigned char c : text) seed = (seed^c)*1099511628211ull; return seed; };
    std::ostringstream stream; stream << std::hex << std::setfill('0') << std::setw(16)
        << hash(key,1469598103934665603ull) << std::setw(16) << hash(key,7809847782465536322ull);
    const auto value = stream.str();
    return value.substr(0,8)+"-"+value.substr(8,4)+"-"+value.substr(12,4)+"-"+value.substr(16,4)+"-"+value.substr(20);
}
// Engine modules are distributed as graph assets. A bare module id from a
// default/legacy system is resolved to the matching asset version; the
// built-in lowering is only reachable from graph capture/generation.
bool ResolveModuleAsset(const SourceLibrary& source,Module& module,Stage stage)
{
    if (module.scriptAsset.Empty() && module.script.empty()) return false;
    if (const auto* script = source.Script(EngineModuleAssetId(module.script)))
        if (const auto* version = SelectVersion(*script,{}); version &&
            (version->graph.nodes.size() || version->stageGraphs.size()) &&
            std::find(version->stages.begin(),version->stages.end(),stage) != version->stages.end())
        { module.scriptAsset = {script->id,version->id}; return true; }
    return false;
}
const SourceLibrary& DefaultEngineLibrary()
{
    static SourceLibrary library;
    static std::once_flag once;
    std::call_once(once,[] {
        std::string error;
        for (const auto& root : {std::filesystem::path("App")/"Assets"/"Engine",
            std::filesystem::path("..")/"App"/"Assets"/"Engine",
            std::filesystem::path("..")/".."/"App"/"Assets"/"Engine"})
        {
            if (!std::filesystem::exists(root)) continue;
            const std::array roots{root};
            if (library.Refresh(roots,error)) break;
        }
    });
    return library;
}
// Emitter graphs only contain literal inputs for now, so they can be reduced
// to their Emitter.* channels at compile time. The runtime keeps consuming the
// resulting schedule until the per-tick emitter program lands.
bool EvaluateEmitterGraph(const Graph& source, std::map<std::string, float>& outputs, std::string& error)
{
    auto graph = source;
    // Emitter state references have a neutral compile-time initial value. A
    // parameter/context-dependent output remains a runtime script output.
    for (auto& node : graph.nodes)
        if (node.operation == Operation::Attribute)
        {
            node.operation = Operation::Constant;
            node.value = {};
        }
    std::vector<Attribute> attributes;
    for (const auto& node : graph.nodes)
        if (node.operation == Operation::WriteAttribute)
            attributes.push_back({node.name, node.type, {}});
    Typed::TypeRegistry types;
    Typed::Graph typed;
    if (!Typed::TranslateGraph(graph, types, attributes, typed, error))
        return false;
    std::unordered_map<Typed::ValueId, std::optional<Typed::Value>> constants;
    for (const auto& node : typed.nodes)
        if (node.operation == Operation::WriteAttribute)
        {
            std::string constantError;
            if (Typed::EvaluateConstant(typed, node.inputs[0], types, constants, constantError))
            {
                const auto& value = *constants.at(node.inputs[0]);
                outputs[node.name] = value.type == Typed::kBool  ? (std::bit_cast<int32_t>(value.scalar[0]) != 0 ? 1.0f : 0.0f) :
                                     value.type == Typed::kInt32 ? static_cast<float>(std::bit_cast<int32_t>(value.scalar[0])) :
                                                                   value.scalar[0];
            }
        }
    error.clear();
    return true;
}

class Builder
{
public:
    Builder(Graph& graph, const System& system, const Emitter& emitter, const Stack& stack, std::vector<Issue>& issues,
        const SourceLibrary& library,std::map<std::string,SourceDependency>& dependencies,std::vector<Attribute>& attributes)
        : graph(graph),system(system),emitter(emitter),stack(stack),issues(issues),library(library),dependencies(dependencies),attributes(attributes) {}
    NodeId Add(Operation operation, ValueType type, std::vector<NodeId> inputs = {}, Value value = {}, std::string name = {})
    {
        if (operation == Operation::NormalizedAge)
        {
            const auto age = Read("Particles.Age",ValueType::Float), lifetime = Read("Particles.Lifetime",ValueType::Float);
            const auto safe = Add(Operation::Maximum,ValueType::Float,{lifetime,Add(Operation::Constant,ValueType::Float,{}, {1e-20f,0,0,0})});
            return Add(Operation::Clamp,ValueType::Float,{Add(Operation::Divide,ValueType::Float,{age,safe}),Add(Operation::Constant,ValueType::Float),Add(Operation::Constant,ValueType::Float,{}, {1,0,0,0})});
        }
        if (graph.nodes.size() >= 65536) { if (!limitReported) { Error("Expanded script exceeds 65536 nodes"); limitReported = true; } return 0; }
        std::vector<ValueType> inputTypes;
        for (const auto input : inputs) if (const auto* node = FindNode(graph,input)) inputTypes.push_back(node->type);
        if (inputTypes.size() == inputs.size() && operation < Operation::StructMake)
        {
            ValueType inferred; std::string error;
            if (ResolveOperationType(operation,type,inputTypes,inferred,error)) type = inferred;
            else Error(error+" (operation "+std::to_string(static_cast<int>(operation))+
                ", inputs "+std::to_string(inputs.size())+"/"+std::to_string(InputCount(operation))+")");
        }
        const auto id = AddNode(graph,operation,type); auto* node = FindNode(graph,id);
        if (!node) { Error("Graph node limit exceeded"); return 0; }
        node->inputs = std::move(inputs); node->value = value; node->name = std::move(name); return id;
    }
    // Registers an attribute the first time a module reads or writes it. This
    // happens lazily so attributes inside inactive static-switch branches are
    // never declared.
    void EnsureAttribute(const std::string& name,ValueType type)
    {
        const auto existing = std::find_if(attributes.begin(),attributes.end(),
            [&](const Attribute& attribute) { return attribute.name == name; });
        if (existing == attributes.end())
        {
            if (attributes.size() >= 1024) { Error("Emitter attribute limit exceeded",name); return; }
            attributes.push_back({name,type,{}});
        }
        else if (existing->type != type) Error("Module requires an incompatible attribute type: "+name,name);
    }
    NodeId Read(const std::string& name, ValueType type)
    {
        if (const auto found = values.find(name); found != values.end()) return found->second;
        return values[name] = Add(Operation::Attribute,type,{}, {},name);
    }
    void Write(const std::string& name, ValueType type, NodeId node) { EnsureAttribute(name,type); values[name] = node; writes[name] = type; writtenAttributes.insert(name); }
    // Seeds a channel default (for example a per-tick transient reset) without
    // counting as a module write for ordering checks.
    void Seed(const std::string& name,ValueType type,NodeId node) { values[name] = node; writes[name] = type; }
    // Read-modify-write accumulation for transient physical channels. The
    // channel is a real particle attribute so force modules can be authored as
    // standalone graphs; the stack zeroes it before modules run each tick.
    void Accumulate(const std::string& name,ValueType type,NodeId value)
    {
        const auto previous = Read(name,type);
        Write(name,type,Add(Operation::Add,type,{previous,value}));
    }
    // Builds a float3 from three scalar nodes by masking basis vectors. The IR
    // has no vector-construction opcode, and adding one would change the
    // serialized Operation enum, so shape modules compose basis terms instead.
    NodeId ComposeVector(NodeId x, NodeId y, NodeId z)
    {
        const auto axis = [&](Value value) { return Add(Operation::Constant,ValueType::Float3,{},value); };
        const auto term = [&](Value value, NodeId scalar)
        { return Add(Operation::Multiply,ValueType::Float3,{axis(value),scalar}); };
        return Add(Operation::Add,ValueType::Float3,{term({1,0,0,0},x),
            Add(Operation::Add,ValueType::Float3,{term({0,1,0,0},y),term({0,0,1,0},z)})});
    }
    // Uniform random in [0,1): Random already produces [-1,1) before scaling.
    NodeId UnitRandom()
    {
        return Add(Operation::Clamp,ValueType::Float,{Add(Operation::Multiply,ValueType::Float,
            {Add(Operation::Random,ValueType::Float,{}, {0.5f,0,0,0}),Add(Operation::Constant,ValueType::Float,{}, {0.5f,0,0,0})}),
            Add(Operation::Constant,ValueType::Float),Add(Operation::Constant,ValueType::Float,{}, {1,0,0,0})});
    }
    // float4 counterpart of ComposeVector, used when a colour must be scaled by
    // a per-axis vector such as velocity.
    NodeId ComposeColor(NodeId x, NodeId y, NodeId z, NodeId w)
    {
        const auto axis = [&](Value value) { return Add(Operation::Constant,ValueType::Float4,{},value); };
        const auto term = [&](Value value, NodeId scalar)
        { return Add(Operation::Multiply,ValueType::Float4,{axis(value),scalar}); };
        return Add(Operation::Add,ValueType::Float4,{term({1,0,0,0},x),
            Add(Operation::Add,ValueType::Float4,{term({0,1,0,0},y),
                Add(Operation::Add,ValueType::Float4,{term({0,0,1,0},z),term({0,0,0,1},w)})})});
    }
    // Component access by projecting onto a basis vector; the IR has no swizzle.
    // The basis width must match the source, so float4 sources pass width 4.
    NodeId Component(NodeId vector, uint32_t index, uint32_t width = 3)
    {
        Value basis{};
        basis[index] = 1;
        const auto type = static_cast<ValueType>(width-1);
        return Add(Operation::Dot,ValueType::Float,{vector,Add(Operation::Constant,type,{},basis)});
    }
    // Quaternion helpers. The renderer consumes xyzw order and renormalises, so
    // composition only has to be algebraically correct.
    NodeId QuaternionFromAxisAngle(NodeId axis, NodeId radians)
    {
        const auto half = Add(Operation::Multiply,ValueType::Float,{radians,
            Add(Operation::Constant,ValueType::Float,{}, {0.5f,0,0,0})});
        const auto sine = Add(Operation::Sine,ValueType::Float,{half});
        const auto cosine = Add(Operation::Cosine,ValueType::Float,{half});
        return ComposeColor(Add(Operation::Multiply,ValueType::Float,{Component(axis,0),sine}),
            Add(Operation::Multiply,ValueType::Float,{Component(axis,1),sine}),
            Add(Operation::Multiply,ValueType::Float,{Component(axis,2),sine}),cosine);
    }
    // Hamilton product a*b for xyzw quaternions.
    NodeId QuaternionMultiply(NodeId a, NodeId b)
    {
        const auto aw = Component(a,3,4), ax = Component(a,0,4), ay = Component(a,1,4), az = Component(a,2,4);
        const auto bw = Component(b,3,4), bx = Component(b,0,4), by = Component(b,1,4), bz = Component(b,2,4);
        const auto sum = [&](std::initializer_list<std::pair<NodeId,NodeId>> terms)
        {
            std::vector<NodeId> products;
            for (const auto& [left,right] : terms)
                products.push_back(Add(Operation::Multiply,ValueType::Float,{left,right}));
            NodeId total = products.front();
            for (size_t i = 1; i < products.size(); ++i)
                total = Add(Operation::Add,ValueType::Float,{total,products[i]});
            return total;
        };
        return ComposeColor(sum({{aw,bx},{ax,bw},{ay,bz},{az,Add(Operation::Multiply,ValueType::Float,{by,
                        Add(Operation::Constant,ValueType::Float,{}, {-1,0,0,0})})}}),
            sum({{aw,by},{ay,bw},{az,bx},{ax,Add(Operation::Multiply,ValueType::Float,{bz,
                        Add(Operation::Constant,ValueType::Float,{}, {-1,0,0,0})})}}),
            sum({{aw,bz},{az,bw},{ax,by},{ay,Add(Operation::Multiply,ValueType::Float,{bx,
                        Add(Operation::Constant,ValueType::Float,{}, {-1,0,0,0})})}}),
            sum({{aw,bw},{ax,Add(Operation::Multiply,ValueType::Float,{bx,
                        Add(Operation::Constant,ValueType::Float,{}, {-1,0,0,0})})},
                 {ay,Add(Operation::Multiply,ValueType::Float,{by,
                        Add(Operation::Constant,ValueType::Float,{}, {-1,0,0,0})})},
                 {az,Add(Operation::Multiply,ValueType::Float,{bz,
                        Add(Operation::Constant,ValueType::Float,{}, {-1,0,0,0})})}}));
    }
    // Rotates a float3 by an xyzw unit quaternion: v = q * v * conj(q) with v
    // treated as a pure quaternion. Used by the mesh-orientation facing module,
    // which needs a real vector rotation rather than a quaternion product.
    NodeId QuaternionRotateVector(NodeId quaternion, NodeId vector)
    {
        const auto negate = [&](NodeId value)
        { return Add(Operation::Multiply,ValueType::Float,{value,Add(Operation::Constant,ValueType::Float,{}, {-1,0,0,0})}); };
        const auto conjugate = ComposeColor(negate(Component(quaternion,0,4)),negate(Component(quaternion,1,4)),
            negate(Component(quaternion,2,4)),Component(quaternion,3,4));
        const auto pure = ComposeColor(Component(vector,0),Component(vector,1),Component(vector,2),
            Add(Operation::Constant,ValueType::Float));
        const auto rotated = QuaternionMultiply(QuaternionMultiply(quaternion,pure),conjugate);
        return ComposeVector(Component(rotated,0,4),Component(rotated,1,4),Component(rotated,2,4));
    }
    // Branches so a degenerate "axis parallel to up" case cannot produce a zero
    // cross product when building a tangent basis.
    void TangentBasis(NodeId axis, NodeId& u, NodeId& v)
    {
        const auto absoluteZ = Add(Operation::Absolute,ValueType::Float,{Component(axis,2)});
        const auto degenerate = Add(Operation::GreaterThan,ValueType::Bool,{absoluteZ,
            Add(Operation::Constant,ValueType::Float,{}, {0.9f,0,0,0})});
        const auto up = Add(Operation::Select,ValueType::Float3,{degenerate,
            Add(Operation::Constant,ValueType::Float3,{}, {1,0,0,0}),
            Add(Operation::Constant,ValueType::Float3,{}, {0,0,1,0})});
        u = Add(Operation::Normalize,ValueType::Float3,{Add(Operation::Cross,ValueType::Float3,{axis,up})});
        v = Add(Operation::Cross,ValueType::Float3,{axis,u});
    }
    // The IR has no power opcode, so the cube root used for uniform volume
    // sampling is solved by bisection over [0,1]. Newton converges far too
    // slowly near zero (over 100% relative error at u=5e-6), while 24 bisection
    // steps bound the relative error at 6.8e-7 across the whole range, well
    // inside the 1e-5 comparison tolerance.
    NodeId CubeRoot(NodeId unit)
    {
        const auto constant = [&](float value) { return Add(Operation::Constant,ValueType::Float,{}, {value,0,0,0}); };
        auto low = constant(0), high = constant(1);
        for (int step = 0; step < 24; ++step)
        {
            const auto middle = Add(Operation::Multiply,ValueType::Float,
                {Add(Operation::Add,ValueType::Float,{low,high}),constant(0.5f)});
            const auto cube = Add(Operation::Multiply,ValueType::Float,{Add(Operation::Multiply,ValueType::Float,{middle,middle}),middle});
            const auto below = Add(Operation::LessThan,ValueType::Bool,{cube,unit});
            low = Add(Operation::Select,ValueType::Float,{below,middle,low});
            high = Add(Operation::Select,ValueType::Float,{below,high,middle});
        }
        return Add(Operation::Multiply,ValueType::Float,{Add(Operation::Add,ValueType::Float,{low,high}),constant(0.5f)});
    }
    void Error(std::string message, std::string input = {}) { issues.push_back({std::move(message),emitter.settings.id,stack.id,module ? module->id : std::string{},std::move(input)}); }
    // Capture mode turns a code-defined module's lowering into an equivalent
    // reusable module graph: inputs become Module.* parameters and the usual
    // Write calls become WriteAttribute nodes in Finish().
    void BeginModuleCapture(const std::string& id,const std::string& required)
    {
        captureModuleGraph = true; seen.insert(id);
        if (!required.empty()) seen.insert(required);
    }
    NodeId InputValue(const std::string& name, ValueType type)
    {
        const auto found = module->inputs.find(name);
        if (found == module->inputs.end()) { Error("Missing module input",name); return Add(Operation::Constant,type); }
        const auto& input = found->second;
        // Module-graph capture: inputs become Module.* parameters so the
        // generated script can be instantiated with different values.
        if (captureModuleGraph)
            return Add(Operation::Parameter,type,{},input.value,"Module."+name);
        if (input.mode == InputMode::Literal)
        { if ((IsIntegralType(input.type) || IsIntegralType(type)) && input.type != type) Error("Literal input type differs from its declaration",name); return Add(Operation::Constant,type,{},input.value); }
        if (input.mode == InputMode::Parameter)
        {
            const auto parameter = std::find_if(system.parameters.begin(),system.parameters.end(),[&](const auto& p) { return p.name == input.parameter; });
            if (parameter == system.parameters.end() || parameter->type != type) { Error("Unknown parameter or incompatible parameter type",name); return Add(Operation::Constant,type); }
            return Add(Operation::Parameter,type,{},parameter->value,parameter->name);
        }
        if (input.mode == InputMode::Curve)
        {
            if (IsIntegralType(type)) { Error("Curves require a floating point input",name); return Add(Operation::Constant,type); }
            const auto constant = [&](Value value, ValueType valueType) { return Add(Operation::Constant,valueType,{},value); };
            auto result = constant(input.curve.front().value,type);
            const auto age = Add(Operation::NormalizedAge,ValueType::Float);
            for (size_t i = 1; i < input.curve.size(); ++i)
            {
                const auto& a = input.curve[i-1]; const auto& b = input.curve[i]; const float duration = b.time-a.time;
                const auto t = Add(Operation::Clamp,ValueType::Float,{
                    Add(Operation::Divide,ValueType::Float,{Add(Operation::Subtract,ValueType::Float,{age,constant({a.time,0,0,0},ValueType::Float)}),constant({duration,0,0,0},ValueType::Float)}),
                    constant({},ValueType::Float),constant({1,0,0,0},ValueType::Float)});
                NodeId segment = 0;
                if (a.interpolation == CurveInterpolation::Linear) segment = Add(Operation::Lerp,type,{constant(a.value,type),constant(b.value,type),t});
                else
                {
                    const auto t2 = Add(Operation::Multiply,ValueType::Float,{t,t}), t3 = Add(Operation::Multiply,ValueType::Float,{t2,t});
                    const auto mul = [&](NodeId value,float scale) { return Add(Operation::Multiply,ValueType::Float,{value,constant({scale,0,0,0},ValueType::Float)}); };
                    const auto h00 = Add(Operation::Add,ValueType::Float,{Add(Operation::Subtract,ValueType::Float,{mul(t3,2),mul(t2,3)}),constant({1,0,0,0},ValueType::Float)});
                    const auto h10 = Add(Operation::Add,ValueType::Float,{Add(Operation::Subtract,ValueType::Float,{t3,mul(t2,2)}),t});
                    const auto h01 = Add(Operation::Subtract,ValueType::Float,{mul(t2,3),mul(t3,2)});
                    const auto h11 = Add(Operation::Subtract,ValueType::Float,{t3,t2});
                    auto arrive = b.arriveTangent, leave = a.leaveTangent; for (size_t c = 0; c < 4; ++c) { arrive[c] *= duration; leave[c] *= duration; }
                    segment = Add(Operation::Add,type,{
                        Add(Operation::Add,type,{Add(Operation::Multiply,type,{h00,constant(a.value,type)}),Add(Operation::Multiply,type,{h10,constant(leave,type)})}),
                        Add(Operation::Add,type,{Add(Operation::Multiply,type,{h01,constant(b.value,type)}),Add(Operation::Multiply,type,{h11,constant(arrive,type)})})});
                }
                result = Add(Operation::Add,type,{result,Add(Operation::Subtract,type,{segment,constant(a.value,type)})});
            }
            return result;
        }
        if (input.mode == InputMode::Script)
        {
            const auto* asset = library.Script(input.scriptAsset.asset); const auto* version = asset ? SelectVersion(*asset,input.scriptAsset.version) : nullptr;
            if (!version || asset->kind == AssetKind::Module) { Error("Dynamic input script or version is missing",name); return Add(Operation::Constant,type); }
            std::map<std::string,NodeId> arguments; std::map<std::string,Value> constants;
            for (const auto& [key,value] : input.scriptArguments) if (std::none_of(version->inputs.begin(),version->inputs.end(),[&](const auto& field) { return field.name == key; })) Error("Unknown dynamic script argument: "+key,name);
            for (const auto& field : version->inputs)
            {
                const auto found = input.scriptArguments.find(field.name); const auto value = found == input.scriptArguments.end() ? field.defaultValue : found->second;
                if (found != input.scriptArguments.end() && IsIntegralType(field.type) && (!input.scriptArgumentTypes.contains(field.name) || input.scriptArgumentTypes.at(field.name) != field.type)) Error("Dynamic argument requires its declared integral type",name);
                arguments[field.name] = Add(Operation::Constant,field.type,{},value); if (field.isStatic) constants[field.name] = value;
            }
            const auto output = ExpandScript(*asset,*version,arguments,constants,false);
            if (const auto* node = FindNode(graph,output); !node || node->type != type) Error("Dynamic script output type does not match its input",name);
            return output;
        }
        const auto& expression = input.expression;
        std::map<NodeId,NodeId> cloned; std::set<NodeId> visiting;
        std::function<NodeId(NodeId)> clone = [&](NodeId id) -> NodeId
        {
            if (cloned.contains(id)) return cloned.at(id);
            const auto* node = FindNode(expression,id);
            if (!node || visiting.size() >= 512 || !visiting.insert(id).second || node->operation == Operation::WriteAttribute)
            { Error("Invalid or cyclic dynamic input",name); return Add(Operation::Constant,type); }
            std::vector<NodeId> inputs; for (const auto dependency : node->inputs) inputs.push_back(clone(dependency));
            const auto result = node->operation == Operation::Attribute && node->structType.empty() ? Read(node->name,node->type) : Add(node->operation,node->type,std::move(inputs),node->value,node->name);
                if (auto* copy = FindNode(graph,result)) copy->structType = node->structType;
            visiting.erase(id); cloned[id] = result; return result;
        };
        const Node* output = nullptr;
        for (const auto& node : expression.nodes) if (node.operation == Operation::WriteAttribute)
        {
            if (output || node.name != "Output" || node.inputs.size() != 1) { Error("Dynamic input requires exactly one Output",name); return Add(Operation::Constant,type); }
            output = &node;
        }
        if (!output || output->type != type) { Error("Dynamic input output type is incompatible",name); return Add(Operation::Constant,type); }
        return clone(output->inputs[0]);
    }
    bool Lower(const Module& item)
    {
        if (!captureModuleGraph)
        {
            if (item.scriptAsset.Empty()) { Error("Module has no asset graph: "+item.script); return false; }
            const auto* asset = library.Script(item.scriptAsset.asset);
            const auto* version = asset ? SelectVersion(*asset,item.scriptAsset.version) : nullptr;
            if (!version || (version->graph.nodes.empty() && version->stageGraphs.empty()))
            { Error("Module asset has no graph: "+item.script); return false; }
            if ((version->capabilities & ModuleCapability_ForceLimit) && !writtenAttributes.contains("Particles.PhysicsForce"))
            { Error("Limit Force requires a preceding force module"); return false; }
            module = &item;
            return LowerScript(item);
        }
        // Graph capture/generation is the only remaining caller of the
        // built-in lowering. Runtime compilation always uses asset graphs.
        module = &item;
        const auto* definition = FindModule(item.script);
        if (!definition) { Error("Unknown module: "+item.script); return false; }
        if (!SupportsStage(*definition,stack.stage)) { Error("Module is incompatible with this stack stage"); return false; }
        for (const auto& [name,input] : item.inputs)
            if (std::none_of(definition->inputs.begin(),definition->inputs.end(),[&](const auto& inputDefinition) { return inputDefinition.name == name; })) Error("Unknown module input",name);
        if (!definition->requiresModule.empty() && !seen.contains(definition->requiresModule)) Error("Requires an earlier "+definition->requiresModule+" module");
        const auto scalar = ValueType::Float, vector = ValueType::Float3, color = ValueType::Float4;
        // Emitter-stage modules write the Emitter.* namespace. The emitter
        // graph is evaluated into the runtime emitter settings at compile time
        // until the per-tick emitter program is wired up.
        if (item.script == "EmitterState")
        {
            Write("Emitter.Duration",scalar,InputValue("Duration",scalar));
            Write("Emitter.Loop",scalar,InputValue("Loop",scalar));
        }
        else if (item.script == "SpawnRate")
        {
            Accumulate("Emitter.SpawnRate",scalar,InputValue("Rate",scalar));
        }
        else if (item.script == "SpawnPerFrame")
        {
            Accumulate("Emitter.SpawnPerFrame",scalar,InputValue("Count",scalar));
        }
        else if (item.script == "SpawnPerUnit")
        {
            Accumulate("Emitter.SpawnPerUnit",scalar,InputValue("Spawn Per Unit",scalar));
        }
        else if (item.script == "CompleteIfUnused")
        {
            Write("Emitter.UnusedGracePeriod",scalar,InputValue("Unused Grace Period",scalar));
        }
        else if (item.script == "SpawnBurst")
        {
            // Niagara-style burst: the emitter script derives the spawn count
            // for this tick from the emitter age. BurstPhase persists in the
            // emitter attribute row so a burst fires once per loop.
            const auto age = Add(Operation::Parameter,scalar,{}, {}, "Emitter.Age");
            const auto time = InputValue("Time",scalar);
            const auto duration = Add(Operation::Maximum,scalar,{Read("Emitter.Duration",scalar),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto loop = Add(Operation::GreaterThan,ValueType::Bool,{Read("Emitter.Loop",scalar),
                Add(Operation::Constant,scalar,{}, {0.5f,0,0,0})});
            const auto after = Add(Operation::GreaterEqual,ValueType::Bool,{age,time});
            const auto cycles = Add(Operation::Convert,scalar,{Add(Operation::Maximum,ValueType::Int32,
                {Add(Operation::Convert,ValueType::Int32,{Add(Operation::Divide,scalar,
                    {Add(Operation::Subtract,scalar,{age,time}),duration})}),Add(Operation::Constant,ValueType::Int32,{}, {0,0,0,0})})});
            const auto oncePhase = Add(Operation::Select,scalar,{after,Add(Operation::Constant,scalar,{}, {1,0,0,0}),
                Add(Operation::Constant,scalar)});
            const auto loopPhase = Add(Operation::Select,scalar,{after,Add(Operation::Add,scalar,{cycles,
                Add(Operation::Constant,scalar,{}, {1,0,0,0})}),Add(Operation::Constant,scalar)});
            const auto phase = Add(Operation::Select,scalar,{loop,loopPhase,oncePhase});
            const auto previous = Read("Emitter.BurstPhase",scalar);
            const auto due = Add(Operation::GreaterThan,ValueType::Bool,{phase,previous});
            Write("Emitter.BurstPhase",scalar,phase);
            Write("Emitter.SpawnCount",scalar,Add(Operation::Select,scalar,{due,InputValue("Count",scalar),
                Add(Operation::Constant,scalar)}));
        }
        else if (item.script == "EventHandlerProperties")
        {
            Write("Emitter.ReceiveSourceIndex",scalar,InputValue("Source Emitter Index",scalar));
            Write("Emitter.ReceiveExecutionMode",scalar,InputValue("Execution Mode",scalar));
            Write("Emitter.ReceiveMaxEvents",scalar,InputValue("Max Events Per Frame",scalar));
        }
        else if (item.script == "GenerateDeathEvent")
        {
            Write("Emitter.GenerateDeath",scalar,InputValue("Event Generation Enabled",scalar));
        }
        else if (item.script == "GenerateLocationEvent")
        {
            Write("Emitter.GenerateSpawn",scalar,InputValue("Event Generation Enabled",scalar));
        }
        else if (item.script == "GenerateCollisionEvent")
        {
            Write("Emitter.GenerateCollision",scalar,InputValue("Event Generation Enabled",scalar));
        }
        else if (item.script == "ReceiveDeathEvent" || item.script == "ReceiveLocationEvent" || item.script == "ReceiveCollisionEvent")
        {
            const float kind = item.script == "ReceiveDeathEvent" ? 2.0f : item.script == "ReceiveLocationEvent" ? 1.0f : 3.0f;
            Write("Emitter.ReceiveKind",scalar,Add(Operation::Constant,scalar,{}, {kind,0,0,0}));
            Write("Emitter.ReceiveSpawnCount",scalar,InputValue("Spawn Count",scalar));
            Write("Emitter.ReceiveRandomAge",scalar,InputValue("Random Age",scalar));
            Write("Emitter.ReceivePosition",vector,Add(Operation::Parameter,vector,{},{},"Emitter.EventPosition"));
            Write("Emitter.ReceiveVelocity",vector,Add(Operation::Parameter,vector,{},{},"Emitter.EventVelocity"));
            Write("Emitter.ReceiveNormal",vector,Add(Operation::Parameter,vector,{},{},"Emitter.EventNormal"));
        }
        else if (item.script == "Collision")
        {
            Write("Emitter.CollisionEnabled",scalar,InputValue("Collision Enabled",scalar));
            Write("Emitter.CollisionRadius",scalar,InputValue("Particle Radius",scalar));
            Write("Emitter.CollisionRestitution",scalar,InputValue("Restitution",scalar));
            Write("Emitter.CollisionFriction",scalar,InputValue("Friction",scalar));
        }
        else if (item.script == "CollisionQueryAndResponse")
        {
            Write("Emitter.CollisionEnabled",scalar,InputValue("Collision Enabled",scalar));
        }
        else if (item.script == "InitializeParticle")
        {
            for (const auto& input : definition->inputs) Write("Particles."+input.name,input.type,InputValue(input.name,input.type));
        }
        else if (item.script == "BoxLocation")
        {
            const auto random = Add(Operation::Random,vector,{}, {1,1,1,0});
            Write("Particles.Position",vector,Add(Operation::Add,vector,{InputValue("Center",vector),Add(Operation::Multiply,vector,{random,InputValue("Extent",vector)})}));
        }
        else if (item.script == "AddVelocity") Write("Particles.Velocity",vector,Add(Operation::Add,vector,{Read("Particles.Velocity",vector),InputValue("Velocity",vector)}));
        else if (item.script == "GravityForce")
        {
            const auto value = InputValue("Acceleration",vector);
            Accumulate("Particles.PhysicsForce",vector,value); pendingForce = true;
        }
        else if (item.script == "Drag")
        {
            const auto value = Add(Operation::Maximum,scalar,{InputValue("Drag",scalar),Add(Operation::Constant,scalar)});
            Accumulate("Particles.PhysicsDrag",scalar,value); pendingDrag = true;
        }
        else if (item.script == "SolveForcesAndVelocity")
        {
            const auto delta = Add(Operation::DeltaTime,scalar);
            auto velocity = Read("Particles.Velocity",vector);
            const auto force = Read("Particles.PhysicsForce",vector);
            const auto drag = Read("Particles.PhysicsDrag",scalar);
            velocity = Add(Operation::Add,vector,{velocity,Add(Operation::Multiply,vector,{force,delta})});
            velocity = Add(Operation::Divide,vector,{velocity,Add(Operation::Add,scalar,{Add(Operation::Constant,scalar,{}, {1,0,0,0}),Add(Operation::Multiply,scalar,{drag,delta})})});
            Write("Particles.Velocity",vector,velocity);
            Write("Particles.Position",vector,Add(Operation::Add,vector,{Read("Particles.Position",vector),Add(Operation::Multiply,vector,{velocity,delta})}));
            // Consumers own the accumulated channels: clear them so a second
            // consumer or the next module does not apply the same force twice.
            Write("Particles.PhysicsForce",vector,Add(Operation::Constant,vector));
            Write("Particles.PhysicsDrag",scalar,Add(Operation::Constant,scalar));
            pendingForce = pendingDrag = false;
        }
        else if (item.script == "ScaleColor") Write("Particles.Color",color,Add(Operation::Multiply,color,{Read("Particles.Color",color),InputValue("Scale",color)}));
        else if (item.script == "ColorOverLife") Write("Particles.Color",color,Add(Operation::Lerp,color,{InputValue("Start",color),InputValue("End",color),Add(Operation::NormalizedAge,scalar)}));
        else if (item.script == "ScaleSize") Write("Particles.Size",scalar,Add(Operation::Multiply,scalar,{Read("Particles.Size",scalar),InputValue("Scale",scalar)}));
        else if (item.script == "OffsetPosition")
            Write("Particles.Position",vector,Add(Operation::Add,vector,{Read("Particles.Position",vector),InputValue("Offset",vector)}));
        else if (item.script == "JitterPosition")
            Write("Particles.Position",vector,Add(Operation::Add,vector,{Read("Particles.Position",vector),
                Add(Operation::Multiply,vector,{Add(Operation::Random,vector,{}, {1,1,1,0}),InputValue("Jitter",vector)})}));
        else if (item.script == "SphereLocation" || item.script == "CylinderLocation")
        {
            // A unit random direction places the particle on the shape surface;
            // the volume case scales that radius by a cube root of a uniform
            // value so the distribution stays uniform inside the shape.
            const auto direction = Add(Operation::Normalize,vector,{Add(Operation::Random,vector,{}, {1,1,1,0})});
            const auto unit = UnitRandom();
            const auto surfaceOnly = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Surface Only",scalar),Add(Operation::Constant,scalar)});
            if (item.script == "SphereLocation")
            {
                const auto volume = Add(Operation::Multiply,scalar,{InputValue("Radius",scalar),
                    CubeRoot(unit)});
                const auto radius = Add(Operation::Select,scalar,{surfaceOnly,InputValue("Radius",scalar),volume});
                Write("Particles.Position",vector,Add(Operation::Add,vector,{InputValue("Center",vector),Add(Operation::Multiply,vector,{direction,radius})}));
            }
            else
            {
                // Cylindrical sampling in local space: a random polar angle
                // around Y, a radius in the XZ plane, and a random height.
                const auto theta = Add(Operation::Multiply,scalar,{Add(Operation::Random,scalar,{}, {1,0,0,0}),
                    Add(Operation::Constant,scalar,{}, {3.14159265f,0,0,0})});
                const auto horizontal = Add(Operation::Multiply,scalar,{InputValue("Radius",scalar),
                    Add(Operation::Select,scalar,{surfaceOnly,Add(Operation::Constant,scalar,{}, {1,0,0,0}),
                        CubeRoot(unit)})});
                const auto height = Add(Operation::Multiply,scalar,{InputValue("Height",scalar),
                    Add(Operation::Subtract,scalar,{unit,Add(Operation::Constant,scalar,{}, {0.5f,0,0,0})})});
                const auto xz = ComposeVector(Add(Operation::Multiply,scalar,{horizontal,Add(Operation::Cosine,scalar,{theta})}),
                    Add(Operation::Constant,scalar),Add(Operation::Multiply,scalar,{horizontal,Add(Operation::Sine,scalar,{theta})}));
                Write("Particles.Position",vector,Add(Operation::Add,vector,{InputValue("Center",vector),
                    Add(Operation::Add,vector,{xz,ComposeVector(Add(Operation::Constant,scalar),height,Add(Operation::Constant,scalar))})}));
            }
        }
        else if (item.script == "ConeLocation")
        {
            const auto direction = Add(Operation::Normalize,vector,{InputValue("Direction",vector)});
            const auto spread = Add(Operation::Multiply,scalar,{InputValue("Angle",scalar),
                Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})});
            const auto jitter = Add(Operation::Multiply,vector,{Add(Operation::Normalize,vector,{Add(Operation::Random,vector,{}, {1,1,1,0})}),spread});
            const auto axis = Add(Operation::Add,vector,{direction,jitter});
            const auto distance = Add(Operation::Multiply,scalar,{UnitRandom(),InputValue("Length",scalar)});
            Write("Particles.Position",vector,Add(Operation::Add,vector,{InputValue("Origin",vector),
                Add(Operation::Multiply,vector,{Add(Operation::Normalize,vector,{axis}),distance})}));
        }
        else if (item.script == "AddVelocityInCone")
        {
            const auto direction = Add(Operation::Normalize,vector,{InputValue("Direction",vector)});
            const auto spread = Add(Operation::Multiply,scalar,{InputValue("Angle",scalar),
                Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})});
            const auto jitter = Add(Operation::Multiply,vector,{Add(Operation::Normalize,vector,{Add(Operation::Random,vector,{}, {1,1,1,0})}),spread});
            const auto velocity = Add(Operation::Multiply,vector,{Add(Operation::Normalize,vector,{Add(Operation::Add,vector,{direction,jitter})}),
                InputValue("Speed",scalar)});
            Write("Particles.Velocity",vector,Add(Operation::Add,vector,{Read("Particles.Velocity",vector),velocity}));
        }
        else if (item.script == "AddVelocityFromPoint")
            Write("Particles.Velocity",vector,Add(Operation::Add,vector,{Read("Particles.Velocity",vector),
                Add(Operation::Multiply,vector,{Add(Operation::Subtract,vector,{Read("Particles.Position",vector),InputValue("Position",vector)}),
                    InputValue("Velocity Scale",scalar)})}));
        else if (item.script == "ScaleVelocity")
            Write("Particles.Velocity",vector,Add(Operation::Multiply,vector,{Read("Particles.Velocity",vector),InputValue("Scale",scalar)}));
        else if (item.script == "AccelerationForce")
        {
            const auto value = InputValue("Acceleration",vector);
            Accumulate("Particles.PhysicsForce",vector,value); pendingForce = true;
        }
        else if (item.script == "PointAttractionForce")
        {
            const auto offset = Add(Operation::Subtract,vector,{InputValue("Attractor Position",vector),Read("Particles.Position",vector)});
            const auto direction = Add(Operation::Normalize,vector,{offset});
            const auto strength = Add(Operation::Divide,scalar,{InputValue("Attraction Strength",scalar),
                Add(Operation::Maximum,scalar,{Add(Operation::Length,scalar,{offset}),Add(Operation::Constant,scalar,{}, {0.01f,0,0,0})})});
            const auto value = Add(Operation::Multiply,vector,{direction,strength});
            Accumulate("Particles.PhysicsForce",vector,value); pendingForce = true;
        }
        else if (item.script == "VortexForce")
        {
            const auto axis = Add(Operation::Normalize,vector,{InputValue("Vortex Axis",vector)});
            const auto offset = Add(Operation::Subtract,vector,{Read("Particles.Position",vector),InputValue("Vortex Origin",vector)});
            const auto tangential = Add(Operation::Cross,vector,{axis,offset});
            const auto value = Add(Operation::Multiply,vector,{Add(Operation::Normalize,vector,{tangential}),InputValue("Vortex Strength",scalar)});
            Accumulate("Particles.PhysicsForce",vector,value); pendingForce = true;
        }
        else if (item.script == "SpriteRotationRate")
        {
            const auto delta = Add(Operation::DeltaTime,scalar);
            const auto rate = Add(Operation::Multiply,scalar,{InputValue("Rotation Rate",scalar),Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})});
            Write("Particles.Rotation",scalar,Add(Operation::Add,scalar,{Read("Particles.Rotation",scalar),Add(Operation::Multiply,scalar,{rate,delta})}));
        }
        else if (item.script == "SubUVAnimation")
        {
            const auto count = Add(Operation::Maximum,scalar,{InputValue("Frame Count",scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto frame = Add(Operation::Multiply,scalar,{Add(Operation::NormalizedAge,scalar),Add(Operation::Subtract,scalar,{count,Add(Operation::Constant,scalar,{}, {1,0,0,0})})});
            const auto start = Add(Operation::Select,scalar,{Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Random Start",scalar),Add(Operation::Constant,scalar)}),
                Add(Operation::Multiply,scalar,{Add(Operation::Random,scalar,{}, {0.5f,0,0,0}),Add(Operation::Subtract,scalar,{count,Add(Operation::Constant,scalar,{}, {1,0,0,0})})}),
                Add(Operation::Constant,scalar)});
            const auto total = Add(Operation::Add,scalar,{InputValue("First Frame",scalar),start});
            Write("Particles.SubImageIndex",scalar,Add(Operation::Clamp,scalar,{
                Add(Operation::Add,scalar,{total,frame}),Add(Operation::Constant,scalar),
                Add(Operation::Subtract,scalar,{Add(Operation::Add,scalar,{InputValue("First Frame",scalar),count}),Add(Operation::Constant,scalar,{}, {1,0,0,0})})}));
        }
        else if (item.script == "SpriteSizeBySpeed")
        {
            const auto speed = Add(Operation::Divide,scalar,{Add(Operation::Length,scalar,{Read("Particles.Velocity",vector)}),
                Add(Operation::Maximum,scalar,{InputValue("Speed Threshold",scalar),Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})})});
            const auto blend = Add(Operation::Clamp,scalar,{speed,Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.Size",scalar,Add(Operation::Multiply,scalar,{Read("Particles.Size",scalar),
                Add(Operation::Lerp,scalar,{InputValue("Min Scale",scalar),InputValue("Max Scale",scalar),blend})}));
        }
        else if (item.script == "TorusLocation")
        {
            // Ring sampling around the given axis: the tube centre sits on the
            // major radius circle, then a random direction in the tube plane
            // adds the minor radius offset.
            const auto axis = Add(Operation::Normalize,vector,{InputValue("Axis",vector)});
            NodeId u = 0, v = 0; TangentBasis(axis,u,v);
            const auto majorAngle = Add(Operation::Multiply,scalar,{UnitRandom(),
                Add(Operation::Constant,scalar,{}, {6.2831853f,0,0,0})});
            const auto radial = Add(Operation::Add,vector,
                {Add(Operation::Multiply,vector,{u,Add(Operation::Cosine,scalar,{majorAngle})}),
                 Add(Operation::Multiply,vector,{v,Add(Operation::Sine,scalar,{majorAngle})})});
            const auto tubeAngle = Add(Operation::Multiply,scalar,{UnitRandom(),
                Add(Operation::Constant,scalar,{}, {6.2831853f,0,0,0})});
            const auto tubeRadial = Add(Operation::Multiply,scalar,
                {Add(Operation::Cosine,scalar,{tubeAngle}),InputValue("Minor Radius",scalar)});
            const auto tubeAlong = Add(Operation::Multiply,scalar,
                {Add(Operation::Sine,scalar,{tubeAngle}),InputValue("Minor Radius",scalar)});
            const auto ring = Add(Operation::Multiply,vector,{radial,InputValue("Major Radius",scalar)});
            const auto tube = Add(Operation::Add,vector,
                {Add(Operation::Multiply,vector,{radial,tubeRadial}),
                 Add(Operation::Multiply,vector,{axis,tubeAlong})});
            Write("Particles.Position",vector,Add(Operation::Add,vector,
                {InputValue("Center",vector),Add(Operation::Add,vector,{ring,tube})}));
        }
        else if (item.script == "WedgeLocation")
        {
            // The wedge widens away from its tip: the cross section half extent
            // scales by 0.5 + 0.5*tan(angle)*along.
            const auto local = Add(Operation::Multiply,vector,
                {Add(Operation::Random,vector,{}, {1,1,1,0}),InputValue("Extent",vector)});
            const auto axis = Add(Operation::Normalize,vector,{InputValue("Axis",vector)});
            const auto along = Add(Operation::Dot,ValueType::Float,{local,axis});
            const auto radians = Add(Operation::Multiply,scalar,{InputValue("Angle",scalar),
                Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})});
            const auto tangent = Add(Operation::Divide,scalar,{Add(Operation::Sine,scalar,{radians}),
                Add(Operation::Maximum,scalar,{Add(Operation::Cosine,scalar,{radians}),
                    Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})})});
            const auto half = Add(Operation::Add,scalar,
                {Add(Operation::Multiply,scalar,{along,tangent}),
                 Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto section = ComposeVector(
                Add(Operation::Multiply,scalar,{Component(local,0),half}),
                Add(Operation::Multiply,scalar,{Component(local,1),half}),
                Add(Operation::Multiply,scalar,{Component(local,2),half}));
            Write("Particles.Position",vector,Add(Operation::Add,vector,
                {InputValue("Origin",vector),section}));
        }
        else if (item.script == "RotateAroundPoint")
        {
            const auto radians = Add(Operation::Multiply,scalar,{InputValue("Angle",scalar),
                Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})});
            const auto offset = Add(Operation::Multiply,vector,
                {ComposeVector(Add(Operation::Cosine,scalar,{radians}),Add(Operation::Constant,scalar),
                    Add(Operation::Sine,scalar,{radians})),InputValue("Radius",scalar)});
            Write("Particles.Position",vector,Add(Operation::Add,vector,{InputValue("Center",vector),offset}));
        }
        else if (item.script == "VortexVelocity")
        {
            const auto axis = Add(Operation::Normalize,vector,{InputValue("Vortex Axis",vector)});
            const auto offset = Add(Operation::Subtract,vector,
                {Read("Particles.Position",vector),InputValue("Vortex Origin",vector)});
            const auto tangential = Add(Operation::Cross,vector,{axis,offset});
            const auto value = Add(Operation::Multiply,vector,
                {Add(Operation::Normalize,vector,{tangential}),InputValue("Vortex Strength",scalar)});
            Write("Particles.Velocity",vector,Add(Operation::Add,vector,{Read("Particles.Velocity",vector),value}));
        }
        else if (item.script == "AlignVelocityToRandomAxis")
        {
            // Keeps the speed and only rotates the direction, so the motion
            // magnitude is preserved exactly as the reference describes.
            const auto speed = Add(Operation::Length,scalar,{Read("Particles.Velocity",vector)});
            const auto blend = Add(Operation::Clamp,scalar,{InputValue("Randomness",scalar),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto axis = Add(Operation::Normalize,vector,{Add(Operation::Random,vector,{}, {1,1,1,0})});
            const auto direction = Add(Operation::Normalize,vector,{Add(Operation::Lerp,vector,
                {Add(Operation::Normalize,vector,{Read("Particles.Velocity",vector)}),axis,blend})});
            Write("Particles.Velocity",vector,Add(Operation::Multiply,vector,{direction,speed}));
        }
        else if (item.script == "ScaleVelocityBySpeed")
        {
            const auto speed = Add(Operation::Length,scalar,{Read("Particles.Velocity",vector)});
            const auto blend = Add(Operation::Clamp,scalar,{Add(Operation::Divide,scalar,{speed,
                Add(Operation::Maximum,scalar,{InputValue("Speed Threshold",scalar),
                    Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})})}),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.Velocity",vector,Add(Operation::Multiply,vector,
                {Read("Particles.Velocity",vector),Add(Operation::Lerp,scalar,
                    {InputValue("Min Scale",scalar),InputValue("Max Scale",scalar),blend})}));
        }
        else if (item.script == "LinearForce" || item.script == "AccelerationForce")
        {
            const auto value = InputValue("Acceleration",vector);
            Accumulate("Particles.PhysicsForce",vector,value); pendingForce = true;
        }
        else if (item.script == "LimitForce")
        {
            if (!captureModuleGraph && !writtenAttributes.contains("Particles.PhysicsForce")) { Error("Limit Force requires a preceding force module"); return false; }
            const auto force = Read("Particles.PhysicsForce",vector);
            const auto magnitude = Add(Operation::Length,scalar,{force});
            const auto excess = Add(Operation::GreaterThan,ValueType::Bool,{magnitude,InputValue("Force Limit",scalar)});
            const auto scale = Add(Operation::Select,scalar,{excess,
                Add(Operation::Divide,scalar,{InputValue("Force Limit",scalar),Add(Operation::Maximum,scalar,{magnitude,
                    Add(Operation::Constant,scalar,{}, {1e-20f,0,0,0})})}),
                Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.PhysicsForce",vector,Add(Operation::Multiply,vector,{force,scale}));
        }
        else if (item.script == "ScaleColorBySpeed")
        {
            const auto speed = Add(Operation::Divide,scalar,
                {Add(Operation::Length,scalar,{Read("Particles.Velocity",vector)}),
                 Add(Operation::Maximum,scalar,{InputValue("Speed Threshold",scalar),
                    Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})})});
            const auto blend = Add(Operation::Clamp,scalar,{speed,
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.Color",color,Add(Operation::Multiply,color,{Read("Particles.Color",color),
                Add(Operation::Lerp,color,{InputValue("Min Scale",color),InputValue("Max Scale",color),blend})}));
        }
        else if (item.script == "ScaleColorByVelocity")
        {
            const auto scale = Add(Operation::Multiply,vector,
                {Read("Particles.Velocity",vector),InputValue("Scale",scalar)});
            Write("Particles.Color",color,Add(Operation::Multiply,color,{Read("Particles.Color",color),
                ComposeColor(Component(scale,0),Component(scale,1),Component(scale,2),
                    Add(Operation::Constant,scalar,{}, {1,0,0,0}))}));
        }
        else if (item.script == "SpriteSizeByVelocity")
        {
            const auto scale = Add(Operation::Multiply,scalar,
                {Add(Operation::Length,scalar,{Read("Particles.Velocity",vector)}),InputValue("Scale",scalar)});
            Write("Particles.Size",scalar,Add(Operation::Multiply,scalar,{Read("Particles.Size",scalar),scale}));
        }
        else if (item.script == "KillParticles")
        {
            // Lifetime below Age makes the existing compaction treat the
            // particle as dead, which is exactly the reference behaviour.
            const auto kill = Add(Operation::GreaterThan,ValueType::Bool,
                {InputValue("Kill",scalar),Add(Operation::Constant,scalar)});
            Write("Particles.Lifetime",scalar,Add(Operation::Select,scalar,{kill,
                Add(Operation::Subtract,scalar,{Read("Particles.Age",scalar),
                    Add(Operation::Constant,scalar,{}, {1,0,0,0})}),
                Read("Particles.Lifetime",scalar)}));
        }        else if (item.script == "ScaleMeshSize")
            Write("Particles.Scale",vector,Add(Operation::Multiply,vector,{Read("Particles.Scale",vector),InputValue("Scale",scalar)}));
        else if (item.script == "ScaleMeshSizeBySpeed")
        {
            const auto speed = Add(Operation::Divide,scalar,{Add(Operation::Length,scalar,{Read("Particles.Velocity",vector)}),
                Add(Operation::Maximum,scalar,{InputValue("Speed Threshold",scalar),Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})})});
            const auto blend = Add(Operation::Clamp,scalar,{speed,Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.Scale",vector,Add(Operation::Multiply,vector,{Read("Particles.Scale",vector),
                Add(Operation::Lerp,scalar,{InputValue("Min Scale",scalar),InputValue("Max Scale",scalar),blend})}));
        }
        else if (item.script == "InitialMeshOrientation")
        {
            const auto axis = Add(Operation::Normalize,vector,{InputValue("Axis",vector)});
            const auto radians = Add(Operation::Multiply,scalar,{InputValue("Angle",scalar),
                Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})});
            Write("Particles.MeshOrientation",color,QuaternionFromAxisAngle(axis,radians));
        }
        else if (item.script == "UpdateMeshOrientation" || item.script == "MeshRotationRate")
        {
            // Update Mesh Orientation derives both the axis and the rate from
            // the angular velocity vector; Mesh Rotation Rate uses its own
            // axis/rate inputs.
            const auto velocity = item.script == "UpdateMeshOrientation"
                ? InputValue("Angular Velocity",vector) : InputValue("Axis",vector);
            const auto axis = Add(Operation::Normalize,vector,{velocity});
            const auto rate = item.script == "UpdateMeshOrientation"
                ? Add(Operation::Length,scalar,{velocity})
                : InputValue("Rate",scalar);
            const auto radians = Add(Operation::Multiply,scalar,{rate,Add(Operation::DeltaTime,scalar)});
            const auto spin = Add(Operation::Multiply,scalar,{radians,Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})});
            Write("Particles.MeshOrientation",color,
                QuaternionMultiply(QuaternionFromAxisAngle(axis,spin),Read("Particles.MeshOrientation",color)));
        }
        else if (item.script == "FrameCounter")
        {
            const auto increment = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Increment Counter",scalar),Add(Operation::Constant,scalar)});
            Write("Particles.FrameCounter",scalar,Add(Operation::Select,scalar,{increment,
                Add(Operation::Add,scalar,{Read("Particles.FrameCounter",scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})}),
                Read("Particles.FrameCounter",scalar)}));
        }
        else if (item.script == "TrackDistanceTraveled")
        {
            const auto speed = Add(Operation::Length,scalar,{Read("Particles.Velocity",vector)});
            Write("Particles.DistanceTraveled",scalar,Add(Operation::Add,scalar,{Read("Particles.DistanceTraveled",scalar),
                Add(Operation::Multiply,scalar,{speed,Add(Operation::Multiply,scalar,{Add(Operation::DeltaTime,scalar),InputValue("Scale",scalar)})})}));
        }
        else if (item.script == "ConeMask")
        {
            const auto axis = Add(Operation::Normalize,vector,{InputValue("Cone Direction",vector)});
            const auto offset = Add(Operation::Subtract,vector,{Read("Particles.Position",vector),InputValue("Cone Origin",vector)});
            const auto distance = Add(Operation::Maximum,scalar,{Add(Operation::Length,scalar,{offset}),Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto alignment = Add(Operation::Divide,scalar,{Add(Operation::Dot,ValueType::Float,{axis,offset}),distance});
            const auto cosine = Add(Operation::Cosine,scalar,{Add(Operation::Multiply,scalar,{InputValue("Cone Angle",scalar),
                Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})})});
            const auto inside = Add(Operation::GreaterEqual,ValueType::Bool,{alignment,cosine});
            Write("Particles.ConeMask",scalar,Add(Operation::Select,scalar,{inside,
                Add(Operation::Constant,scalar,{}, {1,0,0,0}),Add(Operation::Constant,scalar)}));
        }        else if (item.script == "DynamicMaterialParameters")
        {
            // Each slot is written for every binding, including literals: the
            // renderer reads the attribute column, and the definition already
            // declares all four columns for systems that use this module.
            const std::array<const char*,4> inputNames{"Index 0","Index 1","Index 2","Index 3"};
            const std::array<const char*,4> attributeNames{"Particles.DynamicMaterialParameter",
                "Particles.DynamicMaterialParameter1","Particles.DynamicMaterialParameter2",
                "Particles.DynamicMaterialParameter3"};
            for (size_t slot = 0; slot < inputNames.size(); ++slot)
            {
                const auto input = module->inputs.find(inputNames[slot]);
                if (input == module->inputs.end()) continue;
                Write(attributeNames[slot],color,InputValue(inputNames[slot],color));
            }
        }
        else if (item.script == "Color")
        {
            const auto rgb = Add(Operation::Multiply,vector,{InputValue("Color",vector),InputValue("Scale",scalar)});
            const auto alpha = Add(Operation::Multiply,scalar,{InputValue("Alpha",scalar),InputValue("Scale",scalar)});
            Write("Particles.Color",color,ComposeColor(Component(rgb,0),Component(rgb,1),Component(rgb,2),alpha));
        }
        else if (item.script == "ApplyRotationVector")
        {
            const auto velocity = InputValue("Rotational Velocity",vector);
            const auto speed = Add(Operation::Length,scalar,{velocity});
            const auto nonzero = Add(Operation::GreaterThan,ValueType::Bool,{speed,Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto axis = Add(Operation::Normalize,vector,{velocity});
            const auto radians = Add(Operation::Multiply,scalar,{Add(Operation::Multiply,scalar,{speed,Add(Operation::DeltaTime,scalar)}),
                Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})});
            const auto spin = QuaternionFromAxisAngle(axis,radians);
            const auto identity = Add(Operation::Constant,ValueType::Float4,{}, {0,0,0,1});
            const auto applied = QuaternionMultiply(Add(Operation::Select,ValueType::Float4,{nonzero,spin,identity}),
                Read("Particles.MeshOrientation",color));
            Write("Particles.MeshOrientation",color,applied);
        }
        else if (item.script == "IncrementOverTime")
        {
            Write("Particles.IncrementCounter",scalar,Add(Operation::Add,scalar,{Read("Particles.IncrementCounter",scalar),
                Add(Operation::Multiply,scalar,{Add(Operation::DeltaTime,scalar),InputValue("Increment",scalar)})}));
        }
        else if (item.script == "ConstrainVectorToCone")
        {
            // Projects the velocity onto the cone axis; when the angle exceeds the
            // limit the direction is lerped back to the axis by the exact ratio
            // cos(limit)/cos(current), which keeps the constraint continuous.
            const auto velocity = Read("Particles.Velocity",vector);
            const auto speed = Add(Operation::Maximum,scalar,{Add(Operation::Length,scalar,{velocity}),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto axis = Add(Operation::Normalize,vector,{InputValue("Cone Direction",vector)});
            const auto alignment = Add(Operation::Divide,scalar,{Add(Operation::Dot,ValueType::Float,{velocity,axis}),speed});
            const auto cosine = Add(Operation::Cosine,scalar,{Add(Operation::Multiply,scalar,{InputValue("Cone Angle",scalar),
                Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})})});
            const auto outside = Add(Operation::LessThan,ValueType::Bool,{alignment,cosine});
            const auto tangential = Add(Operation::Normalize,vector,{Add(Operation::Subtract,vector,{velocity,
                Add(Operation::Multiply,vector,{axis,Add(Operation::Multiply,scalar,{alignment,speed})})})});
            const auto sine = Add(Operation::SquareRoot,scalar,{Add(Operation::Subtract,scalar,
                {Add(Operation::Constant,scalar,{}, {1,0,0,0}),Add(Operation::Multiply,scalar,{cosine,cosine})})});
            const auto clamped = Add(Operation::Multiply,vector,{Add(Operation::Add,vector,
                {Add(Operation::Multiply,vector,{axis,cosine}),Add(Operation::Multiply,vector,{tangential,sine})}),speed});
            Write("Particles.Velocity",vector,Add(Operation::Select,ValueType::Float3,{outside,clamped,velocity}));
        }
        else if (item.script == "FindClosestPointOnLineSegment")
        {
            const auto start = InputValue("Segment Start",vector), end = InputValue("Segment End",vector);
            const auto segment = Add(Operation::Subtract,vector,{end,start});
            const auto lengthSquared = Add(Operation::Maximum,scalar,{Add(Operation::Dot,ValueType::Float,{segment,segment}),
                Add(Operation::Constant,scalar,{}, {1e-12f,0,0,0})});
            const auto fraction = Add(Operation::Dot,ValueType::Float,
                {Add(Operation::Subtract,vector,{Read("Particles.Position",vector),start}),segment});
            Write("Particles.LineSegmentFraction",scalar,Add(Operation::Clamp,scalar,{Add(Operation::Divide,scalar,{fraction,lengthSquared}),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})}));
        }        else if (item.script == "LineAttractionForce")
        {
            // Closest point on the segment, then a pull towards it.
            const auto start = InputValue("Line Start",vector), end = InputValue("Line End",vector);
            const auto segment = Add(Operation::Subtract,vector,{end,start});
            const auto lengthSquared = Add(Operation::Maximum,scalar,{Add(Operation::Dot,ValueType::Float,{segment,segment}),
                Add(Operation::Constant,scalar,{}, {1e-12f,0,0,0})});
            const auto offset = Add(Operation::Subtract,vector,{Read("Particles.Position",vector),start});
            const auto fraction = Add(Operation::Clamp,scalar,{Add(Operation::Divide,scalar,
                {Add(Operation::Dot,ValueType::Float,{offset,segment}),lengthSquared}),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto closest = Add(Operation::Add,vector,{start,Add(Operation::Multiply,vector,{segment,fraction})});
            const auto value = Add(Operation::Multiply,vector,{Add(Operation::Subtract,vector,{closest,
                Read("Particles.Position",vector)}),InputValue("Attraction Strength",scalar)});
            Accumulate("Particles.PhysicsForce",vector,value); pendingForce = true;
        }
        else if (item.script == "SpringForce")
        {
            const auto offset = Add(Operation::Subtract,vector,{InputValue("Rest Position",vector),
                Read("Particles.Position",vector)});
            const auto spring = Add(Operation::Multiply,vector,{offset,InputValue("Spring",scalar)});
            const auto damping = Add(Operation::Multiply,vector,{Read("Particles.Velocity",vector),
                InputValue("Damping",scalar)});
            const auto value = Add(Operation::Subtract,vector,{spring,damping});
            Accumulate("Particles.PhysicsForce",vector,value); pendingForce = true;
        }
        else if (item.script == "VectorNoiseForce")
        {
            // The noise is sampled from the stable particle identity, so a given
            // particle keeps the same force direction for its whole lifetime.
            const auto noise = Add(Operation::Random,vector,{}, {1,1,1,0});
            const auto value = Add(Operation::Multiply,vector,{noise,InputValue("Noise Strength",vector)});
            Accumulate("Particles.PhysicsForce",vector,value); pendingForce = true;
        }
        else if (item.script == "PointRepulsionForce")
        {
            const auto offset = Add(Operation::Subtract,vector,{Read("Particles.Position",vector),
                InputValue("Repulsor Position",vector)});
            const auto distance = Add(Operation::Length,scalar,{offset});
            const auto falloff = Add(Operation::Select,scalar,{Add(Operation::LessThan,ValueType::Bool,{distance,
                    InputValue("Falloff Radius",scalar)}),
                Add(Operation::Subtract,scalar,{Add(Operation::Constant,scalar,{}, {1,0,0,0}),
                    Add(Operation::Clamp,scalar,{Add(Operation::Divide,scalar,{distance,
                        Add(Operation::Maximum,scalar,{InputValue("Falloff Radius",scalar),
                            Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})})}),
                        Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})})}),
                Add(Operation::Constant,scalar)});
            const auto value = Add(Operation::Multiply,vector,{Add(Operation::Normalize,vector,{offset}),
                Add(Operation::Multiply,scalar,{InputValue("Repulsion Strength",scalar),falloff})});
            Accumulate("Particles.PhysicsForce",vector,value); pendingForce = true;
        }
        else if (item.script == "WindForce")
        {
            // Matches the reference phrasing: blend the current velocity towards
            // the wind velocity at the given scale over one step.
            const auto wind = Add(Operation::Multiply,vector,{InputValue("Wind Velocity",vector),
                InputValue("Wind Scale",scalar)});
            const auto blend = Add(Operation::Clamp,scalar,{Add(Operation::Multiply,scalar,{InputValue("Wind Scale",scalar),
                Add(Operation::DeltaTime,scalar)}),Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.Velocity",vector,Add(Operation::Lerp,vector,{Read("Particles.Velocity",vector),wind,blend}));
        }        else if (item.script == "KillParticlesInVolume")
        {
            // Same lifetime trick as KillParticles, gated on an analytic sphere.
            const auto offset = Add(Operation::Subtract,vector,{Read("Particles.Position",vector),InputValue("Volume Center",vector)});
            const auto inside = Add(Operation::LessThan,ValueType::Bool,{Add(Operation::Length,scalar,{offset}),
                Add(Operation::Maximum,scalar,{InputValue("Volume Radius",scalar),Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})})});
            const auto inverted = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Invert",scalar),Add(Operation::Constant,scalar)});
            // XOR of "inside the sphere" and the invert flag.
            const auto shouldKill = Add(Operation::NotEqual,ValueType::Bool,{inside,inverted});
            Write("Particles.Lifetime",scalar,Add(Operation::Select,scalar,{shouldKill,
                Add(Operation::Subtract,scalar,{Read("Particles.Age",scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})}),
                Read("Particles.Lifetime",scalar)}));
        }
        else if (item.script == "DoOnce")
        {
            // Fires exactly once: the flag latches on the first true trigger.
            // The column is the "already fired" latch: it reads 0 until the first
            // true trigger and stays 1 afterwards. Later modules branch on it
            // with the comparison ops, which is equivalent to the reference's
            // Execute output without inventing a separate transient attribute.
            const auto trigger = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Trigger",scalar),Add(Operation::Constant,scalar)});
            Write("Particles.DoOnce",scalar,Add(Operation::Select,scalar,{trigger,Add(Operation::Constant,scalar,{}, {1,0,0,0}),
                Read("Particles.DoOnce",scalar)}));
        }
        else if (item.script == "CalculateMassAndRotationalInertiaByVolume")
        {
            // Volume approximated as a cube with the particle size as its edge.
            const auto size = Add(Operation::Maximum,scalar,{Read("Particles.Size",scalar),Add(Operation::Constant,scalar)});
            const auto volume = Add(Operation::Multiply,scalar,{Add(Operation::Multiply,scalar,{size,size}),size});
            const auto mass = Add(Operation::Multiply,scalar,{volume,InputValue("Density",scalar)});
            Write("Particles.Mass",scalar,mass);
            // Solid cube inertia about a principal axis: m * s^2 / 6.
            Write("Particles.RotationalInertia",scalar,Add(Operation::Divide,scalar,{
                Add(Operation::Multiply,scalar,{mass,Add(Operation::Multiply,scalar,{size,size})}),
                Add(Operation::Constant,scalar,{}, {6,0,0,0})}));
        }
        else if (item.script == "FindKineticAndPotentialEnergy")
        {
            const auto speedSquared = Add(Operation::Dot,ValueType::Float,{Read("Particles.Velocity",vector),Read("Particles.Velocity",vector)});
            const auto kinetic = Add(Operation::Multiply,scalar,{Add(Operation::Multiply,scalar,{Add(Operation::Constant,scalar,{}, {0.5f,0,0,0}),InputValue("Mass",scalar)}),
                speedSquared});
            Write("Particles.KineticEnergy",scalar,kinetic);
            Write("Particles.TotalEnergy",scalar,Add(Operation::Add,scalar,{kinetic,InputValue("Potential Energy",scalar)}));
        }
        else if (item.script == "RegionMask")
        {
            // Comparisons are scalar-only, so the box test runs per component and
            // the three axis results are combined.
            const auto position = Read("Particles.Position",vector);
            const auto minimum = InputValue("Region Minimum",vector);
            const auto maximum = InputValue("Region Maximum",vector);
            const auto axisOutside = [&](uint32_t axis)
            {
                const auto component = Component(position,axis);
                return Add(Operation::LogicalOr,ValueType::Bool,
                    {Add(Operation::LessThan,ValueType::Bool,{component,Component(minimum,axis)}),
                     Add(Operation::GreaterThan,ValueType::Bool,{component,Component(maximum,axis)})});
            };
            const auto outside = Add(Operation::LogicalOr,ValueType::Bool,{axisOutside(0),
                Add(Operation::LogicalOr,ValueType::Bool,{axisOutside(1),axisOutside(2)})});
            Write("Particles.RegionMask",scalar,Add(Operation::Select,scalar,{outside,Add(Operation::Constant,scalar),
                Add(Operation::Constant,scalar,{}, {1,0,0,0})}));
        }        else if (item.script == "CalculateAccurateVelocity")
        {
            // (position - previousPosition) / delta, then store the new previous
            // position so the next frame has a valid reference.
            const auto position = Read("Particles.Position",vector);
            const auto previous = Read("Particles.PreviousPosition",vector);
            const auto delta = Add(Operation::Maximum,scalar,{Add(Operation::DeltaTime,scalar),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            Write("Particles.Velocity",vector,Add(Operation::Multiply,vector,{
                Add(Operation::Divide,vector,{Add(Operation::Subtract,vector,{position,previous}),delta}),
                InputValue("Velocity Scale",scalar)}));
            Write("Particles.PreviousPosition",vector,position);
        }
        else if (item.script == "InheritVelocity")
            Write("Particles.Velocity",vector,Add(Operation::Add,vector,{Read("Particles.Velocity",vector),
                Add(Operation::Multiply,vector,{InputValue("Source Velocity",vector),InputValue("Inherit Scale",scalar)})}));
        else if (item.script == "ApplyOwnerScaleToAttributes")
        {
            const auto scale = InputValue("Owner Scale",vector);
            Write("Particles.Position",vector,Add(Operation::Multiply,vector,{Read("Particles.Position",vector),scale}));
            // Uniform size follows the largest axis so a non-uniform owner scale
            // does not shrink the sprite on the narrower axes.
            const auto largest = Add(Operation::Maximum,scalar,{Component(scale,0),
                Add(Operation::Maximum,scalar,{Component(scale,1),Component(scale,2)})});
            Write("Particles.Size",scalar,Add(Operation::Multiply,scalar,{Read("Particles.Size",scalar),largest}));
        }
        else if (item.script == "VectorNoiseValue")
        {
            const auto noise = Add(Operation::Add,vector,{Add(Operation::Random,vector,{}, {1,1,1,0}),
                InputValue("Noise Offset",vector)});
            Write("Particles.NoiseValue",vector,Add(Operation::Multiply,vector,{noise,InputValue("Noise Strength",vector)}));
        }
        else if (item.script == "SolveFloatSpringConstraint")
        {
            // Semi-implicit Euler on the spring: v += (k*(rest-x) - c*v)*dt, x += v*dt.
            const auto delta = Add(Operation::DeltaTime,scalar);
            const auto value = Read("Particles.SpringValue",scalar);
            const auto velocity = Read("Particles.SpringVelocity",scalar);
            const auto spring = Add(Operation::Multiply,scalar,{InputValue("Spring",scalar),
                Add(Operation::Subtract,scalar,{InputValue("Rest Value",scalar),value})});
            const auto damping = Add(Operation::Multiply,scalar,{InputValue("Damping",scalar),velocity});
            const auto acceleration = Add(Operation::Subtract,scalar,{spring,damping});
            const auto nextVelocity = Add(Operation::Add,scalar,{velocity,Add(Operation::Multiply,scalar,{acceleration,delta})});
            Write("Particles.SpringVelocity",scalar,nextVelocity);
            Write("Particles.SpringValue",scalar,Add(Operation::Add,scalar,{value,
                Add(Operation::Multiply,scalar,{nextVelocity,delta})}));
        }        else if (item.script == "CameraOffset")
        {
            // Move away from the camera by the scaled particle-to-camera vector.
            const auto cameraPosition = Add(Operation::Parameter,vector,{}, {}, "Camera.Position");
            const auto offset = Add(Operation::Subtract,vector,{cameraPosition,Read("Particles.Position",vector)});
            Write("Particles.Position",vector,Add(Operation::Subtract,vector,{Read("Particles.Position",vector),
                Add(Operation::Multiply,vector,{offset,InputValue("Offset Scale",scalar)})}));
        }
        else if (item.script == "MaintainInCameraParticleScale")
        {
            // Scale so the apparent size stays constant with depth: the ratio of
            // the camera-relative depth to the reference distance, corrected by
            // the tangent of both field of view angles.
            const auto cameraPosition = Add(Operation::Parameter,vector,{}, {}, "Camera.Position");
            const auto toCamera = Add(Operation::Subtract,vector,{Read("Particles.Position",vector),cameraPosition});
            const auto depth = Add(Operation::Absolute,ValueType::Float,{Add(Operation::Dot,ValueType::Float,{toCamera,
                Add(Operation::Parameter,vector,{}, {}, "Camera.Forward")})});
            const auto fov = Add(Operation::Parameter,ValueType::Float,{}, {}, "Camera.FovDegrees");
            const auto half = [&](NodeId degrees)
            { return Add(Operation::Multiply,ValueType::Float,{degrees,
                  Add(Operation::Constant,ValueType::Float,{}, {0.00872664626f,0,0,0})}); };
            const auto tangent = [&](NodeId radians)
            { return Add(Operation::Divide,ValueType::Float,{Add(Operation::Sine,ValueType::Float,{radians}),
                  Add(Operation::Maximum,ValueType::Float,{Add(Operation::Cosine,ValueType::Float,{radians}),
                      Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})})}); };
            const auto scale = Add(Operation::Multiply,ValueType::Float,{
                Add(Operation::Divide,ValueType::Float,{depth,Add(Operation::Maximum,ValueType::Float,
                    {InputValue("Reference Distance",scalar),Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})})}),
                Add(Operation::Divide,ValueType::Float,{tangent(half(InputValue("Reference Fov",scalar))),tangent(half(fov))})});
            Write("Particles.Size",scalar,Add(Operation::Multiply,ValueType::Float,{Read("Particles.Size",scalar),scale}));
        }
        else if (item.script == "ViewRecycler")
        {
            // A particle outside the view is reflected about the camera plane so
            // it re-enters from the opposite side, keeping the population stable
            // without touching the spawn schedule.
            const auto cameraPosition = Add(Operation::Parameter,vector,{}, {}, "Camera.Position");
            const auto forward = Add(Operation::Parameter,vector,{}, {}, "Camera.Forward");
            const auto toCamera = Add(Operation::Subtract,vector,{Read("Particles.Position",vector),cameraPosition});
            const auto depth = Add(Operation::Dot,ValueType::Float,{toCamera,forward});
            const auto margin = Add(Operation::Maximum,ValueType::Float,{InputValue("Recycle Margin",scalar),
                Add(Operation::Constant,ValueType::Float,{}, {1e-4f,0,0,0})});
            // Behind the camera or beyond the margin: mirror the axial offset.
            const auto behind = Add(Operation::LessThan,ValueType::Bool,{depth,margin});
            const auto mirrored = Add(Operation::Add,vector,{cameraPosition,
                Add(Operation::Multiply,vector,{forward,Add(Operation::Subtract,ValueType::Float,
                    {margin,Add(Operation::Subtract,ValueType::Float,{depth,margin})})})});
            Write("Particles.Position",vector,Add(Operation::Select,ValueType::Float3,{behind,mirrored,
                Read("Particles.Position",vector)}));
        }
        else if (item.script == "ViewMask")
        {
            // Project to clip space, then derive the distance to the nearest edge.
            const auto position = Read("Particles.Position",vector);
            const auto toCamera = Add(Operation::Subtract,vector,{position,Add(Operation::Parameter,vector,{}, {}, "Camera.Position")});
            const auto depth = Add(Operation::Maximum,ValueType::Float,{Add(Operation::Dot,ValueType::Float,{toCamera,
                Add(Operation::Parameter,vector,{}, {}, "Camera.Forward")}),Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})});
            const auto halfFov = Add(Operation::Multiply,ValueType::Float,{Add(Operation::Parameter,ValueType::Float,{}, {}, "Camera.FovDegrees"),
                Add(Operation::Constant,ValueType::Float,{}, {0.00872664626f,0,0,0})});
            const auto tangent = Add(Operation::Divide,ValueType::Float,{Add(Operation::Sine,ValueType::Float,{halfFov}),
                Add(Operation::Maximum,ValueType::Float,{Add(Operation::Cosine,ValueType::Float,{halfFov}),
                    Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})})});
            const auto aspect = Add(Operation::Divide,ValueType::Float,{
                Add(Operation::Parameter,ValueType::Float,{}, {}, "Camera.ViewportWidth"),
                Add(Operation::Maximum,ValueType::Float,{Add(Operation::Parameter,ValueType::Float,{}, {}, "Camera.ViewportHeight"),
                    Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})})});
            const auto halfHeight = Add(Operation::Multiply,ValueType::Float,{depth,tangent});
            const auto halfWidth = Add(Operation::Multiply,ValueType::Float,{halfHeight,aspect});
            const auto offsetX = Add(Operation::Absolute,ValueType::Float,{Add(Operation::Dot,ValueType::Float,{toCamera,
                Add(Operation::Parameter,vector,{}, {}, "Camera.Right")})});
            const auto offsetY = Add(Operation::Absolute,ValueType::Float,{Add(Operation::Dot,ValueType::Float,{toCamera,
                Add(Operation::Parameter,vector,{}, {}, "Camera.Up")})});
            const auto normalizedX = Add(Operation::Divide,ValueType::Float,{offsetX,
                Add(Operation::Maximum,ValueType::Float,{halfWidth,Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})})});
            const auto normalizedY = Add(Operation::Divide,ValueType::Float,{offsetY,
                Add(Operation::Maximum,ValueType::Float,{halfHeight,Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})})});
            const auto edge = Add(Operation::Maximum,ValueType::Float,{normalizedX,normalizedY});
            const auto width = Add(Operation::Maximum,ValueType::Float,{InputValue("Edge Width",scalar),
                Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})});
            // Fade from one at the centre to zero at the viewport edge.
            Write("Particles.ViewMask",scalar,Add(Operation::Subtract,ValueType::Float,{Add(Operation::Constant,ValueType::Float,{}, {1,0,0,0}),
                Add(Operation::Clamp,ValueType::Float,{Add(Operation::Divide,ValueType::Float,{edge,width}),
                    Add(Operation::Constant,ValueType::Float),Add(Operation::Constant,ValueType::Float,{}, {1,0,0,0})})}));
            Write("Particles.InsideView",scalar,Add(Operation::Select,scalar,{
                Add(Operation::LessEqual,ValueType::Bool,{edge,Add(Operation::Constant,ValueType::Float,{}, {1,0,0,0})}),
                Add(Operation::Constant,ValueType::Float,{}, {1,0,0,0}),Add(Operation::Constant,ValueType::Float)}));
        }        else if (item.script == "FindClosestPointOnTriangle")
        {
            // Distance from the particle to the triangle plane, which is the
            // closest-point distance for the common co-planar-facing case and a
            // stable analytic metric in general.
            const auto a = InputValue("Vertex A",vector), b = InputValue("Vertex B",vector), c = InputValue("Vertex C",vector);
            const auto normal = Add(Operation::Normalize,vector,{Add(Operation::Cross,vector,
                {Add(Operation::Subtract,vector,{b,a}),Add(Operation::Subtract,vector,{c,a})})});
            const auto toParticle = Add(Operation::Subtract,vector,{Read("Particles.Position",vector),a});
            Write("Particles.TriangleDistance",scalar,Add(Operation::Absolute,ValueType::Float,
                {Add(Operation::Dot,ValueType::Float,{normal,toParticle})}));
        }
        else if (item.script == "CalculateSizeAndRotationalInertiaByMass")
        {
            const auto mass = Add(Operation::Maximum,ValueType::Float,{InputValue("Mass",scalar),Add(Operation::Constant,ValueType::Float)});
            const auto density = Add(Operation::Maximum,ValueType::Float,{InputValue("Density",scalar),
                Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})});
            // Cube of size s has volume s^3, so s = cbrt(mass/density).
            const auto ratio = Add(Operation::Clamp,ValueType::Float,{Add(Operation::Divide,ValueType::Float,{mass,density}),
                Add(Operation::Constant,ValueType::Float),Add(Operation::Constant,ValueType::Float,{}, {1,0,0,0})});
            const auto size = CubeRoot(ratio);
            Write("Particles.Size",scalar,size);
            // Solid cube inertia about a principal axis: m * s^2 / 6.
            Write("Particles.RotationalInertia",scalar,Add(Operation::Divide,ValueType::Float,
                {Add(Operation::Multiply,ValueType::Float,{mass,Add(Operation::Multiply,ValueType::Float,{size,size})}),
                 Add(Operation::Constant,ValueType::Float,{}, {6,0,0,0})}));
        }
        else if (item.script == "AddRotationalVelocity")
        {
            const auto radians = Add(Operation::Multiply,vector,{InputValue("Rotational Velocity",vector),
                Add(Operation::Constant,ValueType::Float,{}, {0.017453292f,0,0,0})});
            Write("Particles.RotationalVelocity",vector,Add(Operation::Add,vector,{Read("Particles.RotationalVelocity",vector),radians}));
        }
        else if (item.script == "UpdateVelocityOnMassChange")
        {
            // Momentum preservation: v_new = v_old * (m_old / m_new).
            const auto previous = Add(Operation::Maximum,ValueType::Float,{InputValue("Previous Mass",scalar),
                Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})});
            const auto current = Add(Operation::Maximum,ValueType::Float,{Read("Particles.Mass",scalar),
                Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})});
            const auto ratio = Add(Operation::Clamp,ValueType::Float,{Add(Operation::Divide,ValueType::Float,{previous,current}),
                Add(Operation::Constant,ValueType::Float),Add(Operation::Constant,ValueType::Float,{}, {100,0,0,0})});
            Write("Particles.Velocity",vector,Add(Operation::Multiply,vector,{Read("Particles.Velocity",vector),ratio}));
        }        else if (item.script == "FirstFrame")
        {
            // Engine.FirstFrame is 1 on the system's first tick and 0 afterwards.
            // The attribute stores the plain float flag so no Bool conversion is
            // needed when a later module compares or lerps it.
            Write("Particles.FirstFrame",scalar,Add(Operation::Select,scalar,{
                Add(Operation::GreaterThan,ValueType::Bool,{Add(Operation::Parameter,ValueType::Float,{}, {}, "Engine.FirstFrame"),
                    Add(Operation::Constant,ValueType::Float)}),
                Add(Operation::Constant,scalar,{}, {1,0,0,0}),Add(Operation::Constant,scalar)}));
        }
        else if (item.script == "EmitterFrameCounter")
        {
            // Emitter-level counter: increments once per system step while enabled.
            const auto enabled = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Increment Counter",scalar),
                Add(Operation::Constant,ValueType::Float)});
            // The counter lives on the particle attribute space so both targets
            // already have storage for it.
            const auto next = Add(Operation::Add,scalar,{Read("Particles.EmitterFrameCounter",scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.EmitterFrameCounter",scalar,Add(Operation::Select,scalar,{enabled,next,Read("Particles.EmitterFrameCounter",scalar)}));
        }
        else if (item.script == "Timeline")
        {
            // Drives a raw timeline from the system age and exposes the
            // normalised value, so curves further down the stack can use it.
            const auto playRate = Add(Operation::Clamp,scalar,{InputValue("Play Rate",scalar),
                Add(Operation::Constant,scalar,{}, {-100,0,0,0}),Add(Operation::Constant,scalar,{}, {100,0,0,0})});
            const auto raw = Add(Operation::Multiply,scalar,{Add(Operation::Parameter,ValueType::Float,{}, {}, "Engine.SystemAge"),playRate});
            const auto length = Add(Operation::Maximum,scalar,{InputValue("Timeline Length",scalar),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto normalized = Add(Operation::Clamp,scalar,{Add(Operation::Divide,scalar,{raw,length}),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            // Raw timeline position is the elapsed driven time itself, so a
            // rewinding timeline reports negative values.
            Write("Particles.TimelinePosition",scalar,raw);
            Write("Particles.TimelineNormalized",scalar,normalized);
            Write("Particles.TimelinePlaying",scalar,Add(Operation::Select,scalar,{
                Add(Operation::GreaterThan,ValueType::Bool,{playRate,Add(Operation::Constant,ValueType::Float)}),
                Add(Operation::Constant,scalar,{}, {1,0,0,0}),Add(Operation::Constant,scalar)}));
        }
        else if (item.script == "TimeBasedStateMachine")
        {
            // Cycle position drives a two-state output: zero while the particle
            // is in the "on" portion of the cycle and one during "off".
            const auto onDuration = Add(Operation::Maximum,ValueType::Float,{InputValue("On Duration",scalar),
                Add(Operation::Constant,ValueType::Float)});
            const auto offDuration = Add(Operation::Maximum,ValueType::Float,{InputValue("Off Duration",scalar),
                Add(Operation::Constant,ValueType::Float)});
            const auto cycleLength = Add(Operation::Maximum,ValueType::Float,
                {Add(Operation::Add,ValueType::Float,{onDuration,offDuration}),
                 Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})});
            const auto scaled = Add(Operation::Multiply,ValueType::Float,
                {Add(Operation::Parameter,ValueType::Float,{}, {}, "Engine.SystemAge"),InputValue("Rate",scalar)});
            const auto cycles = Add(Operation::Divide,ValueType::Float,{scaled,cycleLength});
            // The IR has no floor opcode, so the whole-cycle count comes from an
            // int conversion and back, which truncates towards zero.
            const auto whole = Add(Operation::Convert,ValueType::Float,
                {Add(Operation::Convert,ValueType::Int32,{cycles})});
            const auto cycleTime = Add(Operation::Multiply,ValueType::Float,
                {Add(Operation::Subtract,ValueType::Float,{cycles,whole}),cycleLength});
            // The reference reports state "on" as one during the on window, so
            // the value is high while the cycle time is inside On Duration.
            const auto inOn = Add(Operation::LessThan,ValueType::Bool,{cycleTime,onDuration});
            Write("Particles.OnOffPercentage",scalar,Add(Operation::Select,scalar,{inOn,
                Add(Operation::Constant,scalar,{}, {1,0,0,0}),Add(Operation::Constant,scalar)}));
        }        else if (item.script == "SystemLocation")
        {
            // Spawn offset by the owning system position plus the module offset.
            Write("Particles.Position",vector,Add(Operation::Add,vector,{
                Read("Particles.Position",vector),
                Add(Operation::Add,vector,{Add(Operation::Parameter,vector,{}, {}, "System.Position"),InputValue("Offset",vector)})}));
        }
        else if (item.script == "InheritSystemVelocity")
        {
            Write("Particles.Velocity",vector,Add(Operation::Add,vector,{Read("Particles.Velocity",vector),
                Add(Operation::Multiply,vector,{Add(Operation::Parameter,vector,{}, {}, "System.Velocity"),
                    InputValue("Inherit Scale",scalar)})}));
        }
        else if (item.script == "SystemState")
        {
            const auto age = Add(Operation::Parameter,ValueType::Float,{}, {}, "Engine.SystemAge");
            const auto duration = Add(Operation::Maximum,ValueType::Float,{InputValue("System Duration",scalar),
                Add(Operation::Constant,ValueType::Float,{}, {1e-6f,0,0,0})});
            Write("Particles.SystemAge",scalar,age);
            Write("Particles.SystemNormalizedAge",scalar,Add(Operation::Clamp,ValueType::Float,
                {Add(Operation::Divide,ValueType::Float,{age,duration}),
                 Add(Operation::Constant,ValueType::Float),Add(Operation::Constant,ValueType::Float,{}, {1,0,0,0})}));
        }        else if (item.script == "RibbonWidth" || item.script == "BeamWidth")
        {
            const auto width = Add(Operation::Multiply,scalar,{InputValue("Width",scalar),
                item.script == "BeamWidth" ? InputValue("Scale",scalar) : InputValue("Width Scale",scalar)});
            Write("Particles.RibbonWidth",scalar,width);
        }
        else if (item.script == "ScaleBeamWidth")
        {
            Write("Particles.RibbonWidth",scalar,Add(Operation::Multiply,scalar,{
                Read("Particles.RibbonWidth",scalar),InputValue("Scale",scalar)}));
        }
        else if (item.script == "ScaleRibbonWidth")
        {
            Write("Particles.RibbonWidth",scalar,Add(Operation::Multiply,scalar,{
                Read("Particles.RibbonWidth",scalar),InputValue("Scale",scalar)}));
        }
        else if (item.script == "FadeOverTime")
        {
            const auto minimum = Add(Operation::Maximum,scalar,{InputValue("Minimum Value",scalar),
                Add(Operation::Constant,scalar)});
            if (stack.stage == Stage::ParticleSpawn)
            {
                Write("Particles.FadeValue",scalar,Add(Operation::Maximum,scalar,{InputValue("Initial Value",scalar),minimum}));
                seen.insert(item.script); return true;
            }
            // Expo-style convergence is expressed as an explicit rate-limited
            // step. The lower bound is applied after the target so a transient
            // target below the minimum cannot leak below the configured floor.
            const auto delta = Add(Operation::DeltaTime,scalar);
            const auto rate = Add(Operation::Maximum,scalar,{InputValue("Fade Rate",scalar),
                Add(Operation::Constant,scalar)});
            const auto toward = Add(Operation::Multiply,scalar,{Add(Operation::Subtract,scalar,
                {Read("Particles.FadeValue",scalar),Add(Operation::Maximum,scalar,{InputValue("Target Value",scalar),minimum})}),
                Add(Operation::Clamp,scalar,{Add(Operation::Multiply,scalar,{rate,delta}),
                    Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})})});
            const auto next = Add(Operation::Maximum,scalar,{Add(Operation::Subtract,scalar,
                {Read("Particles.FadeValue",scalar),toward}),minimum});
            // Initial Value is folded into the first step by the spawn graph when
            // it is present; the update graph starts from the column default.
            Write("Particles.FadeValue",scalar,next);
        }
        else if (item.script == "InterpolateOverTime")
        {
            if (stack.stage == Stage::ParticleSpawn)
            {
                Write("Particles.InterpolatedValue",scalar,InputValue("Initial Value",scalar));
                seen.insert(item.script); return true;
            }
            const auto delta = Add(Operation::DeltaTime,scalar);
            const auto rate = Add(Operation::Maximum,scalar,{InputValue("Rate Of Change",scalar),
                Add(Operation::Constant,scalar)});
            const auto distance = Add(Operation::Subtract,scalar,{InputValue("Target Value",scalar),
                Read("Particles.InterpolatedValue",scalar)});
            const auto magnitude = Add(Operation::Absolute,scalar,{distance});
            const auto step = Add(Operation::Multiply,scalar,{rate,delta});
            const auto reached = Add(Operation::GreaterEqual,ValueType::Bool,{step,magnitude});
            const auto sign = Add(Operation::Select,scalar,{Add(Operation::GreaterThan,ValueType::Bool,
                {distance,Add(Operation::Constant,scalar)}),Add(Operation::Constant,scalar,{}, {1,0,0,0}),
                Add(Operation::Constant,scalar,{}, {-1,0,0,0})});
            const auto toward = Add(Operation::Select,scalar,{reached,InputValue("Target Value",scalar),
                Add(Operation::Add,scalar,{Read("Particles.InterpolatedValue",scalar),
                    Add(Operation::Multiply,scalar,{sign,step})})});
            Write("Particles.InterpolatedValue",scalar,toward);
        }
        else if (item.script == "MaintainA_SetDistanceBetweenPoints")
        {
            const auto position = Read("Particles.Position",vector);
            const auto target = InputValue("Target Position",vector);
            const auto offset = Add(Operation::Subtract,vector,{position,target});
            const auto length = Add(Operation::Maximum,scalar,{Add(Operation::Length,scalar,{offset}),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto constrained = Add(Operation::Add,vector,{target,Add(Operation::Multiply,vector,{
                Add(Operation::Divide,vector,{offset,length}),InputValue("Ideal Distance",scalar)})});
            const auto influence = Add(Operation::Clamp,scalar,{InputValue("Influence",scalar),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.Position",vector,Add(Operation::Lerp,vector,{position,constrained,influence}));
        }
        else if (item.script == "PendulumSetup")
        {
            Write("Particles.PendulumPivot",vector,InputValue("Pendulum Pivot",vector));
            Write("Particles.PendulumRestAxis",vector,Add(Operation::Normalize,vector,{InputValue("Pendulum Rest Axis",vector)}));
            Write("Particles.PendulumLength",scalar,InputValue("Pendulum Length",scalar));
            Write("Particles.PendulumSpring",scalar,InputValue("Enable Spring Driver",scalar));
            Write("Particles.PendulumTightness",scalar,InputValue("Spring Tightness",scalar));
        }
        else if (item.script == "PendulumConstraint")
        {
            const auto position = Read("Particles.Position",vector);
            const auto velocity = Read("Particles.Velocity",vector);
            const auto pivot = Read("Particles.PendulumPivot",vector);
            const auto length = Add(Operation::Maximum,scalar,{Read("Particles.PendulumLength",scalar),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto offset = Add(Operation::Subtract,vector,{position,pivot});
            const auto distance = Add(Operation::Maximum,scalar,{Add(Operation::Length,scalar,{offset}),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto freeDirection = Add(Operation::Divide,vector,{offset,distance});
            const auto restAxis = Add(Operation::Normalize,vector,{Read("Particles.PendulumRestAxis",vector)});
            const auto alignment = Add(Operation::Clamp,scalar,{Add(Operation::Dot,ValueType::Float,{freeDirection,restAxis}),
                Add(Operation::Constant,scalar,{}, {-1,0,0,0}),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto angle = Add(Operation::Clamp,scalar,{InputValue("Angle Constraint",scalar),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {180,0,0,0})});
            const auto cosine = Add(Operation::Cosine,scalar,{Add(Operation::Multiply,scalar,{angle,
                Add(Operation::Constant,scalar,{}, {0.017453292f,0,0,0})})});
            const auto sine = Add(Operation::SquareRoot,scalar,{Add(Operation::Maximum,scalar,{Add(Operation::Subtract,scalar,
                {Add(Operation::Constant,scalar,{}, {1,0,0,0}),Add(Operation::Multiply,scalar,{cosine,cosine})}),
                Add(Operation::Constant,scalar)})});
            const auto tangential = Add(Operation::Normalize,vector,{Add(Operation::Subtract,vector,{freeDirection,
                Add(Operation::Multiply,vector,{restAxis,alignment})})});
            const auto limited = Add(Operation::Add,vector,{Add(Operation::Multiply,vector,{restAxis,cosine}),
                Add(Operation::Multiply,vector,{tangential,sine})});
            const auto outside = Add(Operation::LessThan,ValueType::Bool,{alignment,cosine});
            const auto useAngle = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Use Angle Constraint",scalar),
                Add(Operation::Constant,scalar)});
            const auto direction = Add(Operation::Select,vector,{useAngle,
                Add(Operation::Select,vector,{outside,limited,freeDirection}),freeDirection});
            const auto rigid = Add(Operation::Add,vector,{pivot,Add(Operation::Multiply,vector,{direction,length})});
            // The spring driver applies its correction to velocity, while the
            // rigid constraint keeps the endpoint exactly on the sphere. This
            // lets the existing force solver integrate gravity naturally.
            const auto spring = Add(Operation::GreaterThan,ValueType::Bool,{Read("Particles.PendulumSpring",scalar),
                Add(Operation::Constant,scalar)});
            const auto correction = Add(Operation::Subtract,vector,{rigid,position});
            const auto springImpulse = Add(Operation::Multiply,scalar,{Add(Operation::Maximum,scalar,{Read("Particles.PendulumTightness",scalar),
                Add(Operation::Constant,scalar)}),Add(Operation::DeltaTime,scalar)});
            const auto springVelocity = Add(Operation::Add,vector,{velocity,
                Add(Operation::Multiply,vector,{correction,springImpulse})});
            const auto finalVelocity = Add(Operation::Select,vector,{spring,springVelocity,velocity});
            Write("Particles.Velocity",vector,finalVelocity);
            Write("Particles.Position",vector,rigid);
        }
        else if (item.script == "PointForce")
        {
            const auto offset = Add(Operation::Subtract,vector,{Read("Particles.Position",vector),InputValue("Force Origin",vector)});
            const auto distance = Add(Operation::Maximum,scalar,{Add(Operation::Length,scalar,{offset}),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto radial = Add(Operation::Divide,vector,{offset,distance});
            const auto falloffDistance = Add(Operation::Maximum,scalar,{InputValue("Falloff Distance",scalar),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto falloff = Add(Operation::Multiply,scalar,{Add(Operation::Select,scalar,{Add(Operation::GreaterThan,ValueType::Bool,
                {InputValue("Use Falloff",scalar),Add(Operation::Constant,scalar)}),Add(Operation::Subtract,scalar,
                    {Add(Operation::Constant,scalar,{}, {1,0,0,0}),Add(Operation::Clamp,scalar,{Add(Operation::Divide,scalar,
                        {distance,falloffDistance}),Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})})}),
                Add(Operation::Constant,scalar,{}, {1,0,0,0})}),Add(Operation::Select,scalar,
                    {Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Invert Falloff",scalar),Add(Operation::Constant,scalar)}),
                     Add(Operation::Constant,scalar,{}, {-1,0,0,0}),Add(Operation::Constant,scalar,{}, {1,0,0,0})})});
            const auto value = Add(Operation::Multiply,vector,{radial,Add(Operation::Multiply,scalar,{
                InputValue("Force Strength",scalar),falloff})});
            Accumulate("Particles.PhysicsForce",vector,value); pendingForce = true;
        }
        else if (item.script == "InheritSourceMovement")
        {
            if (stack.stage == Stage::ParticleSpawn)
            {
                Write("Particles.Velocity",vector,Add(Operation::Multiply,vector,{InputValue("Source Velocity",vector),
                    InputValue("Applied Force Scale",scalar)}));
                seen.insert(item.script); return true;
            }
            Write("Particles.Velocity",vector,Add(Operation::Add,vector,{Read("Particles.Velocity",vector),
                Add(Operation::Multiply,vector,{InputValue("Source Velocity",vector),InputValue("Applied Force Scale",scalar)})}));
        }
        else if (item.script == "BeamEmitterSetup")
        {
            const auto start = InputValue("Beam Start",vector), end = InputValue("Beam End",vector);
            const auto startTangent = Add(Operation::Normalize,vector,{InputValue("Beam Start Tangent",vector)});
            const auto endTangent = Add(Operation::Normalize,vector,{InputValue("Beam End Tangent",vector)});
            const auto order = Add(Operation::Clamp,scalar,{Read("Particles.RibbonLinkOrder",scalar),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto useTangents = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Use Beam Tangents",scalar),
                Add(Operation::Constant,scalar)});
            const auto startWeight = Add(Operation::Select,scalar,{useTangents,Add(Operation::Subtract,scalar,
                {Add(Operation::Constant,scalar,{}, {1,0,0,0}),order}),Add(Operation::Constant,scalar)});
            const auto endWeight = Add(Operation::Select,scalar,{useTangents,order,Add(Operation::Constant,scalar)});
            const auto linear = Add(Operation::Lerp,vector,{start,end,order});
            const auto curved = Add(Operation::Add,vector,{linear,Add(Operation::Add,vector,{
                Add(Operation::Multiply,vector,{startTangent,startWeight}),
                Add(Operation::Multiply,vector,{endTangent,endWeight})})});
            Write("Particles.Position",vector,Add(Operation::Select,vector,{useTangents,curved,linear}));
            Write("Particles.RibbonLinkOrder",scalar,order);
            Write("Particles.RibbonWidth",scalar,InputValue("Width",scalar));
            Write("Particles.BeamStart",vector,start);
            Write("Particles.BeamEnd",vector,end);
            Write("Particles.BeamStartTangent",vector,startTangent);
            Write("Particles.BeamEndTangent",vector,endTangent);
            Write("Particles.BeamTangentWeight",scalar,startWeight);
        }
        else if (item.script == "ParticleState")
        {
            // The host advances Age before the update graph executes. This
            // module publishes NormalizedAge and applies the optional loop and
            // kill policies without introducing a second state column.
            const auto age = Read("Particles.Age",scalar);
            const auto lifetime = Add(Operation::Maximum,scalar,{Read("Particles.Lifetime",scalar),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto looping = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Loop Particles Lifetime",scalar),
                Add(Operation::Constant,scalar)});
            const auto kill = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Kill Particles When Lifetime Has Elapsed",scalar),
                Add(Operation::Constant,scalar)});
            const auto wrapped = Add(Operation::Subtract,scalar,{age,lifetime});
            const auto expired = Add(Operation::GreaterEqual,ValueType::Bool,{age,lifetime});
            const auto looped = Add(Operation::Select,scalar,{expired,wrapped,age});
            const auto normalized = Add(Operation::Clamp,scalar,{Add(Operation::Divide,scalar,{age,lifetime}),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto killLifetime = Add(Operation::Subtract,scalar,{age,
                Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.Age",scalar,Add(Operation::Select,scalar,{looping,looped,age}));
            Write("Particles.NormalizedAge",scalar,normalized);
            Write("Particles.Lifetime",scalar,Add(Operation::Select,scalar,{kill,killLifetime,lifetime}));
        }        else if (item.script == "SpriteBasedLine")
        {
            const auto pointA = Read("Particles.Position",vector);
            const auto pointB = InputValue("Point B",vector);
            const auto delta = Add(Operation::Subtract,vector,{pointB,pointA});
            const auto length = Add(Operation::Maximum,scalar,{Add(Operation::Length,scalar,{delta}),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto half = Add(Operation::Constant,scalar,{}, {0.5f,0,0,0});
            const auto center = Add(Operation::Multiply,vector,{Add(Operation::Add,vector,{pointA,pointB}),half});
            const auto sizeX = Add(Operation::Maximum,scalar,{length,half});
            const auto sizeY = InputValue("Sprite Width",scalar);
            Write("Particles.Position",vector,center);
            // Sprite ABI stores one scalar size. Use the segment-length scale
            // so velocity-aligned sprites stretch along their long axis; the
            // width remains a renderer-side constant until a two-axis size ABI
            // is introduced.
            Write("Particles.Size",scalar,sizeX);
            Write("Particles.Velocity",vector,delta);
        }
        else if (item.script == "LerpParticleAttributes")
        {
            const auto alpha = Add(Operation::Clamp,scalar,{InputValue("Lerp Alpha",scalar),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.Position",vector,Add(Operation::Lerp,vector,{
                InputValue("Position A",vector),InputValue("Position B",vector),alpha}));
            Write("Particles.Velocity",vector,Add(Operation::Lerp,vector,{
                InputValue("Velocity A",vector),InputValue("Velocity B",vector),alpha}));
            Write("Particles.Color",color,Add(Operation::Lerp,color,{
                InputValue("Color A",color),InputValue("Color B",color),alpha}));
        }
        else if (item.script == "GridLocation")
        {
            // SpawnIndex is the exact ordinal in the accepted spawn batch.
            // Decode X-major / Y / Z with Int32 division so the ordinal is
            // consumed exactly; floating point division would leak the
            // remainder into the upper axes.
            const auto scalarI = ValueType::Int32;
            const auto ordinal = Add(Operation::SpawnIndex,scalarI);
            const auto count = [&](const char* name)
            {
                const auto value = Add(Operation::Maximum,scalar,{InputValue(name,scalar),
                    Add(Operation::Constant,scalar,{}, {1,0,0,0})});
                const auto whole = Add(Operation::Convert,scalarI,{value});
                return Add(Operation::Maximum,scalarI,{whole,Add(Operation::Constant,scalarI,{}, IntValue(1))});
            };
            const auto xCount = count("Grid X Count");
            const auto yCount = count("Grid Y Count");
            const auto zCount = count("Grid Z Count");
            const auto plane = Add(Operation::Multiply,scalarI,{xCount,yCount});
            const auto zMax = Add(Operation::Maximum,scalarI,{Add(Operation::Subtract,scalarI,{zCount,
                Add(Operation::Constant,scalarI,{}, IntValue(1))}),Add(Operation::Constant,scalarI)});
            const auto z = Add(Operation::Minimum,scalarI,{Add(Operation::Divide,scalarI,{ordinal,plane}),zMax});
            const auto inPlane = Add(Operation::Subtract,scalarI,{ordinal,Add(Operation::Multiply,scalarI,{z,plane})});
            const auto yMax = Add(Operation::Maximum,scalarI,{Add(Operation::Subtract,scalarI,{yCount,
                Add(Operation::Constant,scalarI,{}, IntValue(1))}),Add(Operation::Constant,scalarI)});
            const auto y = Add(Operation::Minimum,scalarI,{Add(Operation::Divide,scalarI,{inPlane,xCount}),yMax});
            const auto x = Add(Operation::Subtract,scalarI,{inPlane,Add(Operation::Multiply,scalarI,{y,xCount})});
            const auto cell = ComposeVector(Add(Operation::Convert,scalar,{x}),
                Add(Operation::Convert,scalar,{y}),Add(Operation::Convert,scalar,{z}));
            const auto dimensions = InputValue("XYZ Dimensions",vector);
            const auto jitter = Add(Operation::Multiply,vector,{Add(Operation::Random,vector,{}, {1,1,1,0}),
                InputValue("Jitter Inside Grid Cells",scalar)});
            Write("Particles.Position",vector,Add(Operation::Add,vector,{InputValue("Grid Origin",vector),
                Add(Operation::Multiply,vector,{Add(Operation::Add,vector,{cell,jitter}),dimensions})}));
        }        else if (item.script == "SpawnParticlesInGrid")
        {
            const auto ordinal = Add(Operation::SpawnIndex,scalar);
            const auto xCount = Add(Operation::Maximum,scalar,{InputValue("X Count",scalar),
                Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto yCount = Add(Operation::Maximum,scalar,{InputValue("Y Count",scalar),
                Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto zCount = Add(Operation::Maximum,scalar,{InputValue("Z Count",scalar),
                Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            Write("Particles.GridCellIndex",scalar,ordinal);
            Write("Particles.GridCellAge",scalar,InputValue("Age",scalar));
            // Persist the birth ordinal so update-stage modules (for example
            // Partition Particles) can reuse it after the spawn graph ends.
            Write("Particles.SpawnIndex",scalar,ordinal);
            // Age is also the intrinsic lifetime clock; spawn-time age begins
            // at the requested value and the normal update path advances it.
            Write("Particles.Age",scalar,InputValue("Age",scalar));
        }
        else if (item.script == "PartitionParticles")
        {
            // Partitions are derived from the persisted birth ordinal, so the
            // assignment is stable across ticks instead of reshuffling per
            // frame. Int32 division keeps the quotient/remainder exact; the
            // remainder over Particles Per Partition is the normalized
            // position inside the current partition.
            const auto scalarI = ValueType::Int32;
            const auto ordinal = Add(Operation::Convert,scalarI,{Read("Particles.SpawnIndex",scalar)});
            const auto partitionCount = Add(Operation::Maximum,scalar,{InputValue("Number Of Partitions",scalar),
                Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto perPartition = Add(Operation::Maximum,scalar,{InputValue("Particles Per Partition",scalar),
                Add(Operation::Constant,scalar,{}, {1,0,0,0})});
            const auto partitions = Add(Operation::Maximum,scalarI,{Add(Operation::Convert,scalarI,{partitionCount}),
                Add(Operation::Constant,scalarI,{}, IntValue(1))});
            const auto perPartitionI = Add(Operation::Maximum,scalarI,{Add(Operation::Convert,scalarI,{perPartition}),
                Add(Operation::Constant,scalarI,{}, IntValue(1))});
            const auto span = Add(Operation::Multiply,scalarI,{partitions,perPartitionI});
            // Wrap the ordinal into the configured partition space so a batch
            // larger than NumberOfPartitions x ParticlesPerPartition reuses
            // partitions instead of overrunning them.
            const auto wrapped = Add(Operation::Subtract,scalarI,{ordinal,Add(Operation::Multiply,scalarI,
                {Add(Operation::Divide,scalarI,{ordinal,span}),span})});
            const auto partition = Add(Operation::Divide,scalarI,{wrapped,perPartitionI});
            const auto within = Add(Operation::Subtract,scalarI,{wrapped,Add(Operation::Multiply,scalarI,{partition,perPartitionI})});
            Write("Particles.Partition",scalar,Add(Operation::Convert,scalar,{partition}));
            Write("Particles.PartitionNormalized",scalar,Add(Operation::Clamp,scalar,{
                Add(Operation::Divide,scalar,{Add(Operation::Convert,scalar,{within}),perPartition}),
                Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})}));
        }
        else if (item.script == "SpriteFacingAndAlignment")
        {
            Write("Particles.SpriteFacing",vector,InputValue("Facing",vector));
            Write("Particles.SpriteAlignment",vector,InputValue("Alignment",vector));
        }
        else if (item.script == "AlignSpriteToMeshOrientation")
        {
            // Rotate the chosen local axis by the particle's mesh orientation so
            // the sprite faces whatever the mesh faces. v' = q * v * conj(q),
            // with v as a pure quaternion; the helper is defined next to the
            // other quaternion builders.
            const auto axis = Add(Operation::Normalize,vector,{InputValue("Axis",vector)});
            const auto orientation = Read("Particles.MeshOrientation",color);
            Write("Particles.SpriteFacing",vector,QuaternionRotateVector(orientation,axis));
        }        else if (item.script == "MeshRotationForce")
        {
            const auto rotation = Add(Operation::Multiply,vector,{InputValue("Rotation",vector),
                InputValue("Lever Radius",scalar)});
            Accumulate("Particles.PhysicsRotationalForce",vector,rotation);
        }
        else if (item.script == "ApplyInitialForces")
        {
            const auto toVelocity = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Apply Force to Velocity",scalar),
                Add(Operation::Constant,scalar)});
            const auto toPosition = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Apply Force to Position",scalar),
                Add(Operation::Constant,scalar)});
            const auto rotational = Add(Operation::GreaterThan,ValueType::Bool,{InputValue("Apply Rotational Force to Rotational Velocity",scalar),
                Add(Operation::Constant,scalar)});
            const auto mass = Add(Operation::Maximum,scalar,{Read("Particles.Mass",scalar),
                Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})});
            const auto force = Read("Particles.PhysicsForce",vector);
            const auto impulse = Add(Operation::Divide,vector,{force,mass});
            Write("Particles.Velocity",vector,Add(Operation::Select,vector,{toVelocity,
                Add(Operation::Add,vector,{Read("Particles.Velocity",vector),impulse}),
                Read("Particles.Velocity",vector)}));
            const auto warmup = Add(Operation::Multiply,scalar,{InputValue("Positional Force Warmup Time",scalar),
                Add(Operation::Constant,scalar,{}, {0.5f,0,0,0})});
            Write("Particles.Position",vector,Add(Operation::Select,vector,{toPosition,
                Add(Operation::Add,vector,{Read("Particles.Position",vector),
                    Add(Operation::Multiply,vector,{impulse,warmup})}),
                Read("Particles.Position",vector)}));
            const auto rotationalForce = Read("Particles.PhysicsRotationalForce",vector);
            const auto spin = Add(Operation::Divide,vector,{rotationalForce,mass});
            Write("Particles.RotationalVelocity",vector,Add(Operation::Select,vector,{rotational,
                Add(Operation::Add,vector,{Read("Particles.RotationalVelocity",vector),spin}),
                Read("Particles.RotationalVelocity",vector)}));
            // Consumers own the accumulated force: clearing it here keeps a
            // later Solve Forces and Velocity from applying it twice.
            Write("Particles.PhysicsForce",vector,Add(Operation::Constant,vector));
            Write("Particles.PhysicsRotationalForce",vector,Add(Operation::Constant,vector));
            pendingForce = false;
        }
        else if (item.script == "SpawnBeam" || item.script == "UpdateBeam")
        {
            // RibbonLinkOrder drives the along-strip parameter, so it has to be
            // stable per particle. The scheduler assigns ids in birth order and
            // SpawnBeam places particles in that same order, so the order is the
            // particle's own index modulo the segment count.
            const auto start = InputValue("Start",vector), end = InputValue("End",vector);
            if (item.script == "SpawnBeam")
            {
                // The spawn program runs before any position exists, so the
                // order is derived from the link order column the module owns.
                Write("Particles.Position",vector,start);
                Write("Particles.RibbonLinkOrder",scalar,Add(Operation::Divide,scalar,
                    {Read("Particles.RibbonLinkOrder",scalar),Add(Operation::Maximum,scalar,{InputValue("Particles Per Unit",scalar),
                        Add(Operation::Constant,scalar,{}, {1e-6f,0,0,0})})}));
                Write("Particles.RibbonWidth",scalar,Add(Operation::Constant,scalar,{}, {0.1f,0,0,0}));
            }
            else
            {
                const auto order = Add(Operation::Clamp,scalar,{Read("Particles.RibbonLinkOrder",scalar),
                    Add(Operation::Constant,scalar),Add(Operation::Constant,scalar,{}, {1,0,0,0})});
                Write("Particles.Position",vector,Add(Operation::Lerp,vector,{start,end,order}));
            }
        }        else { Error("Module requires an emitter execution context"); return false; }
        seen.insert(item.script); return true;
    }
    void Finish()
    {
        // Captured module graphs are standalone: the solver lives in a later
        // module, so the ordering check only applies to a stack compile.
        if (!captureModuleGraph && (pendingForce || pendingDrag)) Error("Force and drag modules require a following Solve Forces and Velocity module");
        for (const auto& [name,type] : writes) Add(Operation::WriteAttribute,type,{values.at(name)}, {},name);
    }
    NodeId ExpandScript(const ScriptAsset& asset,const ScriptVersion& version,
        const std::map<std::string,NodeId>& arguments,const std::map<std::string,Value>& staticArguments,bool moduleOutput)
    {
        const auto key = asset.id+":"+version.id;
        if (calls.size() >= 32 || !calls.insert(key).second) { Error("Function dependency cycle or call depth exceeds 32"); return 0; }
        struct Pop { std::set<std::string>& calls; std::string key; ~Pop() { calls.erase(key); } } pop{calls,key};
        if (const auto dependency = library.Dependency(asset.id)) dependencies[asset.id] = *dependency;
        if (std::find(version.stages.begin(),version.stages.end(),stack.stage) == version.stages.end()) { Error("Script version does not support this execution stage"); return 0; }
        const auto& sourceGraph = ScriptGraph(version,stack.stage);
        std::string declarationError;
        if (!MergeGraphDeclarations(graph,sourceGraph,declarationError)) { Error(declarationError); return 0; }
        std::map<NodeId,NodeId> cloned; std::set<NodeId> visiting;
        std::function<NodeId(NodeId,uint32_t)> clone = [&](NodeId id,uint32_t depth) -> NodeId
        {
            if (cloned.contains(id)) return cloned.at(id);
            const auto* node = FindNode(sourceGraph,id);
            if (!node || depth > 512 || !visiting.insert(id).second || node->operation == Operation::WriteAttribute) { Error("Invalid or cyclic script expression"); return Add(Operation::Constant,ValueType::Float); }
            NodeId result = 0;
            const auto branch = std::find_if(version.switches.begin(),version.switches.end(),[&](const auto& value) { return value.node == id; });
            const auto call = std::find_if(version.calls.begin(),version.calls.end(),[&](const auto& value) { return value.node == id; });
            if (branch != version.switches.end())
            {
                const auto input = staticArguments.find(branch->input);
                if (input == staticArguments.end()) Error("Static switch is missing its compile-time input: "+branch->input);
                else
                {
                    const auto field = std::find_if(version.inputs.begin(),version.inputs.end(),[&](const auto& f) { return f.name == branch->input; });
                    const bool enabled = field != version.inputs.end() && field->type == ValueType::Bool ? ReadBool(input->second) : input->second[0] != 0;
                    result = clone(enabled ? branch->whenTrue : branch->whenFalse,depth+1);
                }
            }
            else if (call != version.calls.end())
            {
                const auto* function = library.Script(call->script.asset); const auto* selected = function ? SelectVersion(*function,call->script.version) : nullptr;
                if (!selected || function->kind == AssetKind::Module) Error("Function asset or pinned version is missing or incompatible");
                else
                {
                    std::map<std::string,NodeId> inputs; std::map<std::string,Value> constants;
                    for (const auto& [name,value] : call->arguments)
                        if (std::none_of(selected->inputs.begin(),selected->inputs.end(),[&](const auto& input) { return input.name == name; })) Error("Unknown function argument: "+name);
                    for (const auto& input : selected->inputs)
                    {
                        const auto supplied = call->arguments.find(input.name);
                        if (supplied == call->arguments.end()) { inputs[input.name] = Add(Operation::Constant,input.type,{},input.defaultValue); if (input.isStatic) constants[input.name] = input.defaultValue; }
                        else
                        {
                            inputs[input.name] = clone(supplied->second,depth+1);
                        if (const auto* argument = FindNode(graph,inputs[input.name]); argument && (IsIntegralType(argument->type) || IsIntegralType(input.type)) && argument->type != input.type) Error("Function argument requires an explicit conversion: "+input.name);
                            if (input.isStatic)
                            {
                                const auto* value = FindNode(sourceGraph,supplied->second);
                            if (value && (IsIntegralType(input.type) || IsIntegralType(value->type)) && value->type != input.type) Error("Function argument requires an explicit conversion: "+input.name);
                                if (value && value->operation == Operation::Constant) constants[input.name] = value->value;
                                else if (value && value->operation == Operation::Parameter && value->name.starts_with("Module.") && staticArguments.contains(value->name.substr(7))) constants[input.name] = staticArguments.at(value->name.substr(7));
                                else Error("Static function arguments must be compile-time constants");
                            }
                        }
                    }
                    result = ExpandScript(*function,*selected,inputs,constants,false);
                }
            }
            else if (node->operation == Operation::Parameter && node->name.starts_with("Module."))
            {
                const auto input = arguments.find(node->name.substr(7));
                if (input == arguments.end()) Error("Unknown module input: "+node->name); else result = input->second;
            }
            else
            {
                std::vector<NodeId> inputs; for (const auto dependency : node->inputs) inputs.push_back(clone(dependency,depth+1));
                result = node->operation == Operation::Attribute && node->structType.empty() ? Read(node->name,node->type) : Add(node->operation,node->type,std::move(inputs),node->value,node->name);
                if (auto* copy = FindNode(graph,result)) copy->structType = node->structType;
            }
            visiting.erase(id); cloned[id] = result ? result : Add(Operation::Constant,node->type); return cloned[id];
        };
        NodeId result = 0;
        // All module expressions see the incoming map. Commit writes together
        // after expansion, matching the existing graph's read-before-write rule.
        std::vector<std::tuple<std::string,ValueType,NodeId>> outputs;
        for (const auto& node : sourceGraph.nodes) if (node.operation == Operation::WriteAttribute)
        {
            if (node.inputs.size() != 1) { Error("Script output is disconnected"); continue; }
            const auto value = clone(node.inputs[0],0);
            if (moduleOutput && !node.structType.empty())
            {
                std::function<void(const std::string&,const std::string&,NodeId)> fields;
                fields=[&](const std::string& name,const std::string& typeName,NodeId bundle)
                {
                    const auto type=std::find_if(graph.structs.begin(),graph.structs.end(),[&](const auto& type){return type.name==typeName;});
                    if(type==graph.structs.end()){Error("Unknown struct output type: "+typeName);return;}
                    // Adding nodes may reallocate graph storage, so iterate a copy.
                    const auto definitions=type->fields;
                    for(const auto& field:definitions)
                    {
                        const auto selected=Add(Operation::StructGet,field.type,{bundle},{},field.name);
                        if(field.structType.empty())outputs.emplace_back(name+"."+field.name,field.type,selected);
                        else fields(name+"."+field.name,field.structType,selected);
                    }
                };
                fields(node.name,node.structType,value);
            }
            else if (moduleOutput) outputs.emplace_back(node.name,node.type,value); else if (node.name == "Output") result = value;
        }
        for (const auto& [name,type,value] : outputs)
        {
            if (moduleOutput)
            {
                if (name == "Particles.PhysicsForce") pendingForce = true;
                if (name == "Particles.PhysicsDrag") pendingDrag = true;
            }
            Write(name,type,value);
        }
        return result;
    }
    bool LowerScript(const Module& item)
    {
        const auto* asset = library.Script(item.scriptAsset.asset);
        const auto* version = asset ? SelectVersion(*asset,item.scriptAsset.version) : nullptr;
        if (!version || asset->kind != AssetKind::Module) { Error("Module asset or pinned version is missing or incompatible"); return false; }
        std::map<std::string,NodeId> arguments; std::map<std::string,Value> constants;
        for (const auto& [name,input] : item.inputs)
            if (std::none_of(version->inputs.begin(),version->inputs.end(),[&](const auto& definition) { return definition.name == name; })) Error("Input was removed or renamed in the selected script version",name);
        // Materialize only the inputs the graph actually reads. Unused
        // defaults would otherwise add constant nodes and shift instruction
        // order relative to the captured module graph.
        std::set<std::string> referenced;
        for (const auto& node : ScriptGraph(*version,stack.stage).nodes)
            if (node.operation == Operation::Parameter && node.name.starts_with("Module."))
                referenced.insert(node.name.substr(7));
        // Static inputs are compile-time values, and a switch's input may only
        // appear in the inactive branch, so always materialize them.
        for (const auto& input : version->inputs) if (input.isStatic) referenced.insert(input.name);
        for (const auto& name : referenced)
            if (std::none_of(version->inputs.begin(),version->inputs.end(),[&](const auto& input) { return input.name == name; }))
                Error("Unknown module input: "+name);
        for (const auto& input : version->inputs)
        {
            if (!referenced.contains(input.name)) continue;
            if (!item.inputs.contains(input.name))
            {
                arguments[input.name] = Add(Operation::Constant,input.type,{},input.defaultValue);
                if (input.isStatic) constants[input.name] = input.defaultValue;
            }
            else
            {
                arguments[input.name] = InputValue(input.name,input.type);
                if (input.isStatic)
                { if (item.inputs.at(input.name).mode != InputMode::Literal) Error("Static module inputs must be literal values",input.name); else constants[input.name] = item.inputs.at(input.name).value; }
            }
        }
        for (const auto& requirement : version->requiresBefore) if (!seen.contains(requirement)) Error("Requires an earlier dependency: "+requirement);
        ExpandScript(*asset,*version,arguments,constants,true);
        // Consumers declare which transient channels they consume so an
        // earlier force module does not stay pending.
        if (version->capabilities & ModuleCapability_ConsumesForce) pendingForce = false;
        if (version->capabilities & ModuleCapability_ConsumesDrag) pendingDrag = false;
        seen.insert(asset->id);
        seen.insert(asset->name);
        seen.insert(item.script);
        for (const auto& provided : version->provides) seen.insert(provided); return true;
    }
    Graph& graph;
    const System& system;
    const Emitter& emitter;
    const Stack& stack;
    std::vector<Issue>& issues;
    const Module* module = nullptr;
    std::map<std::string,NodeId> values;
    std::map<std::string,ValueType> writes;
    std::set<std::string> writtenAttributes;
    std::set<std::string> seen;
    // force/drag are consumed by Solve Forces and Velocity. rotationalForce is
    // consumed by Apply Initial Forces, mirroring Niagara's
    // Transient.PhysicsRotationalForce channel.
    bool pendingForce = false, pendingDrag = false;
    bool limitReported = false;
    const SourceLibrary& library;
    std::map<std::string,SourceDependency>& dependencies;
    std::vector<Attribute>& attributes;
    std::set<std::string> calls;
    bool captureModuleGraph = false;
};
}

bool Compile(const System& source, CompiledSystem& output, std::vector<Issue>& issues)
{
    SourceLibrary library; std::vector<SourceDependency> dependencies;
    return Compile(source,library,output,dependencies,issues);
}
bool Compile(const System& source,const SourceLibrary& library,CompiledSystem& output,std::vector<SourceDependency>& dependencies,std::vector<Issue>& issues,System* resolved,CompileCache* cache)
{
    if (!Validate(source,issues)) return false;
    System materialized; std::vector<SourceDependency> emitterDependencies;
    SourceLibrary effective = library;
    if (!effective.Resolve(source,materialized,emitterDependencies,issues)) return false;
    struct PendingModule { Module* module; Stage stage; std::string emitter; };
    std::vector<PendingModule> unresolved;
    for (auto& emitter : materialized.emitters)
        for (auto& stack : emitter.stacks)
        {
            if (!stack.enabled) continue;
            for (auto& module : stack.modules)
                if (module.enabled && module.scriptAsset.Empty() && !module.script.empty() &&
                    !ResolveModuleAsset(effective,module,stack.stage))
                    unresolved.push_back({&module,stack.stage,emitter.settings.id});
        }
    if (!unresolved.empty())
    {
        const auto& engine = DefaultEngineLibrary();
        for (const auto& [id,script] : engine.Scripts())
            if (!effective.Script(id))
            { std::string ignored; effective.Add(script,{},ignored); }
        for (const auto& pending : unresolved)
            if (!ResolveModuleAsset(effective,*pending.module,pending.stage))
            {
                issues.push_back({"Module has no engine asset graph: "+pending.module->script,pending.emitter,{},pending.module->id});
                return false;
            }
    }
    std::map<std::string,SourceDependency> captured; for (const auto& value : emitterDependencies) captured[value.asset] = value;
    SystemAsset lowered; lowered.id = source.id; lowered.name = source.name;
    lowered.version = source.version;
    lowered.systemAttributes = materialized.systemAttributes;
    lowered.systemSpawn = materialized.systemSpawn;
    lowered.systemUpdate = materialized.systemUpdate;
    lowered.timing = materialized.timing;
    lowered.completion = materialized.completion;
    for (const auto& parameter : materialized.parameters) lowered.parameters.push_back({parameter.name,parameter.type,parameter.value});
    // Camera-relative modules read reserved Camera.* parameters. They are only
    // declared when a module actually references them, so systems that do not
    // use the camera keep their previous parameter set and program identity.
    // The camera modules below reference these parameters directly, so a stack
    // that contains a module with the Camera capability declares the whole set.
    const auto moduleCapabilities = [&](const Module& module)
    {
        if (module.scriptAsset.Empty())
        {
            const auto* definition = FindModule(module.script);
            return definition ? definition->capabilities : uint32_t{};
        }
        const auto* asset = effective.Script(module.scriptAsset.asset);
        const auto* version = asset ? SelectVersion(*asset,module.scriptAsset.version) : nullptr;
        return version ? version->capabilities : uint32_t{};
    };
    const auto hasCapability = [&](ModuleCapability capability)
    {
        for (const auto& emitter : materialized.emitters)
            for (const auto& stack : emitter.stacks)
                for (const auto& module : stack.modules)
                    if (moduleCapabilities(module) & capability) return true;
        return false;
    };
    const auto usesCameraModule = hasCapability(ModuleCapability_Camera);
    const auto needsCameraValue = [&](std::string_view) { return usesCameraModule; };
    // Reserved engine values: system frame index, first-frame flag and system age.
    // They are published by the host before each tick, like the camera set.
    const auto usesEngineValue = hasCapability(ModuleCapability_EngineValue);
    for (const auto& [name,type,fallback] : std::initializer_list<std::tuple<const char*,ValueType,Value>>{
            {"Camera.Position",ValueType::Float3,{}},{"Camera.Forward",ValueType::Float3,{0,0,1,0}},
            {"Camera.Right",ValueType::Float3,{1,0,0,0}},{"Camera.Up",ValueType::Float3,{0,1,0,0}},
            {"Camera.FovDegrees",ValueType::Float,{60,0,0,0}},
            {"Camera.ViewportWidth",ValueType::Float,{1920,0,0,0}},{"Camera.ViewportHeight",ValueType::Float,{1080,0,0,0}},
            {"Engine.FirstFrame",ValueType::Float,{1,0,0,0}},{"Engine.SystemAge",ValueType::Float,{}},
            {"Engine.FrameIndex",ValueType::Float,{}},
            {"System.Position",ValueType::Float3,{}},{"System.Velocity",ValueType::Float3,{}}})
        if ((needsCameraValue(name) || (usesEngineValue && (std::string_view{name}.starts_with("Engine.") ||
            std::string_view{name}.starts_with("System.")))) &&
            std::none_of(lowered.parameters.begin(),lowered.parameters.end(),
            [&](const ParameterDefinition& p) { return p.name == name; }))
            lowered.parameters.push_back({name,type,fallback});
    for (size_t emitterIndex = 0; emitterIndex < materialized.emitters.size(); ++emitterIndex)
    {
        const auto& emitter = materialized.emitters[emitterIndex];
        if (!emitter.enabled) continue;
        auto settings = emitter.settings;
        settings.spawn.nodes.clear(); settings.spawn.nextNodeId = 1;
        settings.update.nodes.clear(); settings.update.nextNodeId = 1;
        settings.spawnRate = 0; settings.bursts.clear(); settings.simulationStages.clear();
        // Modules declare the attributes they read or write; register any that
        // the emitter does not have yet so a stack can introduce renderer inputs
        // such as Particles.Rotation without hand-editing the attribute list.
        // Graph-only attributes are registered lazily while lowering, which
        // keeps inactive static-switch branches undeclared.
        const auto registerAttribute = [&](const std::string& name,ValueType type,const Value& defaultValue,
            const std::string& stackId,const std::string& moduleId)
        {
            const auto existing = std::find_if(settings.attributes.begin(),settings.attributes.end(),
                [&](const Attribute& attribute) { return attribute.name == name; });
            if (existing == settings.attributes.end())
            {
                if (settings.attributes.size() >= 1024)
                { issues.push_back({"Emitter attribute limit exceeded",settings.id,stackId,moduleId}); return; }
                settings.attributes.push_back({name,type,defaultValue});
            }
            else if (existing->type != type)
                issues.push_back({"Module requires an incompatible attribute type: "+name,settings.id,stackId,moduleId,name});
        };
        for (const auto& stack : emitter.stacks)
        {
            if (!stack.enabled) continue;
            for (const auto& module : stack.modules)
            {
                if (!module.enabled) continue;
                if (module.scriptAsset.Empty())
                {
                    if (const auto* definition = FindModule(module.script))
                        for (const auto& required : definition->attributes)
                            registerAttribute(required.name,required.type,required.defaultValue,stack.id,module.id);
                }
                else if (const auto* asset = effective.Script(module.scriptAsset.asset))
                    if (const auto* version = SelectVersion(*asset,module.scriptAsset.version))
                        for (const auto& required : version->attributes)
                            registerAttribute(required.name,required.type,required.defaultValue,stack.id,module.id);
            }
        }
        // Transient physical channels are particle attributes so force modules
        // can be authored as graphs; they must start each tick at zero.
        const auto zeroTransientAttributes = [&](Builder& builder,const Stack& stack)
        {
            for (const auto& attribute : settings.attributes)
            {
                if (attribute.name != "Particles.PhysicsForce" && attribute.name != "Particles.PhysicsDrag" &&
                    attribute.name != "Particles.PhysicsRotationalForce") continue;
                const bool used = std::any_of(stack.modules.begin(),stack.modules.end(),[&](const auto& module)
                {
                    if (!module.enabled) return false;
                    if (module.scriptAsset.Empty())
                    {
                        const auto* definition = FindModule(module.script);
                        return definition && std::any_of(definition->attributes.begin(),definition->attributes.end(),
                            [&](const auto& required) { return required.name == attribute.name; });
                    }
                    const auto* asset = effective.Script(module.scriptAsset.asset);
                    const auto* version = asset ? SelectVersion(*asset,module.scriptAsset.version) : nullptr;
                    if (!version) return false;
                    if (std::any_of(version->attributes.begin(),version->attributes.end(),
                        [&](const auto& required) { return required.name == attribute.name; })) return true;
                    const auto& usedGraph = ScriptGraph(*version,stack.stage);
                    return std::any_of(usedGraph.nodes.begin(),usedGraph.nodes.end(),[&](const auto& node)
                    { return (node.operation == Operation::Attribute || node.operation == Operation::WriteAttribute) && node.name == attribute.name; });
                });
                if (used) builder.Seed(attribute.name,attribute.type,builder.Add(Operation::Constant,attribute.type,{}));
            }
        };
        for (const auto& stack : emitter.stacks)
        {
            if (stack.stage == Stage::Simulation)
            {
                SimulationStage stage; stage.id = stack.id; stage.name = stack.name; stage.enabled = stack.enabled; stage.iterations = stack.iterations;
                stage.bindings = stack.bindings;
                const auto bind = [&](const std::string& name,bool boolean)
                {
                    if (name.empty()) return;
                    const auto parameter = std::find_if(materialized.parameters.begin(),materialized.parameters.end(),[&](const auto& value) { return value.name == name; });
                    const auto required = boolean ? ValueType::Bool : ValueType::Int32;
                    if (parameter == materialized.parameters.end() || (parameter->type != required && parameter->type != ValueType::Float))
                    { issues.push_back({"Stage control requires a scalar User parameter: "+name,settings.id,stack.id}); return; }
                    const bool typed = parameter->type == required;
                    stage.bindings.typed |= typed;
                    const auto value = typed ? (boolean ? (ReadBool(parameter->value) ? 1.0 : 0.0) : static_cast<double>(ReadInt(parameter->value))) : static_cast<double>(parameter->value[0]);
                    if ((boolean && value != 0 && value != 1) || (!boolean && (value > 128 || std::floor(value) != value)))
                    { issues.push_back({boolean ? "Enabled Binding default must be 0 or 1" : "Num Iterations default must be an integer no greater than 128",settings.id,stack.id}); return; }
                    if (boolean) stage.bindings.enabledDefault = value != 0;
                    else stage.iterations = value <= 0 ? 0u : static_cast<uint32_t>(value);
                };
                bind(stage.bindings.enabled,true); bind(stage.bindings.iterations,false);
                if (stage.bindings.typed)
                    for (const auto& name : {stage.bindings.enabled,stage.bindings.iterations}) if (!name.empty())
                    {
                        const auto p = std::find_if(materialized.parameters.begin(),materialized.parameters.end(),[&](const auto& value) { return value.name == name; });
                        if (p != materialized.parameters.end() && p->type == ValueType::Float) issues.push_back({"Typed stage controls require Bool for Enabled and Int32 for Num Iterations",settings.id,stack.id});
                    }
                stage.graph.id = stack.id; stage.graph.stage = ScriptStage::SimulationStage;
                if (settings.target != SimulationTarget::GPU) issues.push_back({"Simulation Stages require GPU simulation",settings.id,stack.id});
                if (stack.enabled)
                {
                    Builder builder(stage.graph,materialized,emitter,stack,issues,effective,captured,settings.attributes);
                    zeroTransientAttributes(builder,stack);
                    for (const auto& module : stack.modules) if (module.enabled) builder.Lower(module);
                    builder.Finish();
                }
                settings.simulationStages.push_back(std::move(stage));
                continue;
            }
            if (!stack.enabled) continue;
            if (stack.stage == Stage::ParticleSpawn || stack.stage == Stage::ParticleUpdate)
            {
                auto& graph = stack.stage == Stage::ParticleSpawn ? settings.spawn : settings.update;
                graph.id = stack.id;
                Builder builder(graph,materialized,emitter,stack,issues,effective,captured,settings.attributes);
                zeroTransientAttributes(builder,stack);
                // Partitioning reads the persisted birth ordinal. Populate it
                // in the spawn graph whenever an update-stage module consumes
                // it, so authors do not have to add a second module by hand.
                const bool partitionsParticles = stack.stage == Stage::ParticleUpdate &&
                    std::any_of(stack.modules.begin(),stack.modules.end(),[&](const auto& module)
                    { return module.enabled && (moduleCapabilities(module) & ModuleCapability_PartitionParticles); });
                if (partitionsParticles && settings.attributes.end() == std::find_if(
                    settings.attributes.begin(),settings.attributes.end(),
                    [](const Attribute& attribute) { return attribute.name == "Particles.SpawnIndex"; }))
                    settings.attributes.push_back({"Particles.SpawnIndex",ValueType::Float,{}});
                if (partitionsParticles && graph.stage == ScriptStage::ParticleUpdate)
                {
                    // A spawn-stage module may already own this column (for
                    // example Spawn Particles in Grid). Attributes accept a
                    // single writer, so only synthesize the write when the
                    // spawn graph has not declared one.
                    const bool spawnWritesIndex = std::any_of(settings.spawn.nodes.begin(),settings.spawn.nodes.end(),
                        [](const Node& node) { return node.operation == Operation::WriteAttribute &&
                            node.name == "Particles.SpawnIndex"; });
                    if (!spawnWritesIndex)
                    {
                        auto& spawnGraph = settings.spawn;
                        spawnGraph.id = spawnGraph.id.empty() ? stack.id : spawnGraph.id;
                        Builder spawnBuilder(spawnGraph,materialized,emitter,stack,issues,effective,captured,settings.attributes);
                        spawnBuilder.Write("Particles.SpawnIndex",ValueType::Float,
                            spawnBuilder.Add(Operation::SpawnIndex,ValueType::Float));
                        spawnBuilder.Finish();
                    }
                }
                const auto isCollisionModule = [&](const Module& module)
                {
                    if (moduleCapabilities(module) & ModuleCapability_Collision) return true;
                    if (module.scriptAsset.Empty()) return false;
                    const auto* asset = effective.Script(module.scriptAsset.asset);
                    const auto* version = asset ? SelectVersion(*asset,module.scriptAsset.version) : nullptr;
                    return version && std::any_of(version->graph.nodes.begin(),version->graph.nodes.end(),[](const auto& node)
                    { return node.operation == Operation::WriteAttribute && node.name == "Emitter.CollisionEnabled"; });
                };
                const bool hasCollision = stack.stage == Stage::ParticleUpdate &&
                    std::any_of(stack.modules.begin(),stack.modules.end(),[&](const auto& module) { return module.enabled && isCollisionModule(module); });
                if (hasCollision)
                {
                    auto& collisionGraph = settings.emitterCollision;
                    collisionGraph.id = ScopedGraphId(stack.id+":collision");
                    collisionGraph.stage = ScriptStage::ParticleUpdate;
                    collisionGraph.nodes.clear(); collisionGraph.nextNodeId = 1;
                    std::vector<Attribute> collisionAttributes = settings.emitterAttributes;
                    const auto ensureCollisionAttribute = [&](const char* name,const Value& defaultValue)
                    {
                        if (std::none_of(collisionAttributes.begin(),collisionAttributes.end(),
                            [&](const Attribute& attribute) { return attribute.name == name; }))
                            collisionAttributes.push_back({name,ValueType::Float,defaultValue});
                    };
                    ensureCollisionAttribute("Emitter.CollisionEnabled",{});
                    ensureCollisionAttribute("Emitter.CollisionRadius",{0.05f,0,0,0});
                    ensureCollisionAttribute("Emitter.CollisionRestitution",{0.5f,0,0,0});
                    ensureCollisionAttribute("Emitter.CollisionFriction",{});
                    Builder collisionBuilder(collisionGraph,materialized,emitter,stack,issues,effective,captured,collisionAttributes);
                    for (const auto& module : stack.modules)
                        if (module.enabled && isCollisionModule(module)) collisionBuilder.Lower(module);
                    collisionBuilder.Finish();
                    settings.emitterAttributes = collisionAttributes;
                    const bool dynamic = std::any_of(collisionGraph.nodes.begin(),collisionGraph.nodes.end(),
                        [](const auto& node) { return node.operation == Operation::Parameter; });
                    if (!dynamic)
                    {
                        std::map<std::string,float> outputs; std::string evaluateError;
                        if (!EvaluateEmitterGraph(collisionGraph,outputs,evaluateError))
                            issues.push_back({evaluateError,settings.id,stack.id});
                        else
                        {
                            const auto value = [&](const char* name,float& target)
                            {
                                const auto it = outputs.find(name);
                                if (it == outputs.end()) return false;
                                target = it->second; return true;
                            };
                            float enabled = 0, radius = settings.collisionRadius, restitution = settings.restitution, friction = settings.friction;
                            if (value("Emitter.CollisionEnabled",enabled)) settings.collisionEnabled = enabled != 0;
                            if (value("Emitter.CollisionRadius",radius))
                            {
                                if (!std::isfinite(radius) || radius < 0 || radius > 10000) issues.push_back({"Collision radius must be between 0 and 10000",settings.id,stack.id});
                                else settings.collisionRadius = radius;
                            }
                            if (value("Emitter.CollisionRestitution",restitution))
                            {
                                if (!std::isfinite(restitution) || restitution < 0 || restitution > 1) issues.push_back({"Collision restitution must be between 0 and 1",settings.id,stack.id});
                                else settings.restitution = restitution;
                            }
                            if (value("Emitter.CollisionFriction",friction))
                            {
                                if (!std::isfinite(friction) || friction < 0 || friction > 1) issues.push_back({"Collision friction must be between 0 and 1",settings.id,stack.id});
                                else settings.friction = friction;
                            }
                        }
                    }
                }
                for (const auto& module : stack.modules) if (module.enabled)
                {
                    if (isCollisionModule(module)) continue;
                    builder.Lower(module);
                }
                builder.Finish();
            }
            else if (stack.stage == Stage::EmitterUpdate || stack.stage == Stage::EmitterSpawn)
            {
                auto& emitterGraph = stack.stage == Stage::EmitterUpdate ? settings.emitterUpdate : settings.emitterSpawn;
                emitterGraph.id = stack.id;
                emitterGraph.stage = stack.stage == Stage::EmitterUpdate ? ScriptStage::EmitterUpdate : ScriptStage::EmitterSpawn;
                emitterGraph.nodes.clear(); emitterGraph.nextNodeId = 1;
                std::vector<Attribute> emitterAttributes = settings.emitterAttributes;
                if (emitterAttributes.empty())
                    emitterAttributes = {
                        {"Emitter.SpawnRate",ValueType::Float,{}},{"Emitter.SpawnPerFrame",ValueType::Float,{}},
                        {"Emitter.SpawnPerUnit",ValueType::Float,{}},{"Emitter.Duration",ValueType::Float,{5,0,0,0}},
                        {"Emitter.Loop",ValueType::Float,{1,0,0,0}},{"Emitter.UnusedGracePeriod",ValueType::Float,{}},
                        {"Emitter.BurstPhase",ValueType::Float,{}},{"Emitter.SpawnCount",ValueType::Float,{}}};
                else
                {
                    // Older sources may not carry the full emitter schema.
                    const auto ensure = [&](const char* name,const Value& defaultValue)
                    {
                        if (std::none_of(emitterAttributes.begin(),emitterAttributes.end(),
                            [&](const Attribute& attribute) { return attribute.name == name; }))
                            emitterAttributes.push_back({name,ValueType::Float,defaultValue});
                    };
                    ensure("Emitter.SpawnRate",{}); ensure("Emitter.SpawnPerFrame",{}); ensure("Emitter.SpawnPerUnit",{});
                    ensure("Emitter.Duration",{5,0,0,0}); ensure("Emitter.Loop",{1,0,0,0}); ensure("Emitter.UnusedGracePeriod",{});
                    ensure("Emitter.BurstPhase",{}); ensure("Emitter.SpawnCount",{});
                }
                for (auto& attribute : emitterAttributes)
                {
                    if (attribute.name == "Emitter.Duration")
                        attribute.defaultValue = {static_cast<float>(settings.duration),0,0,0};
                    else if (attribute.name == "Emitter.Loop")
                        attribute.defaultValue = {settings.looping ? 1.0f : 0.0f,0,0,0};
                }
                // Seed the additive schedule channels with the emitter's base
                // values so the graph output is absolute. Literal graphs are
                // baked into settings; dynamic graphs run on the emitter
                // scheduler at tick time.
                const auto baseSpawnRate = static_cast<float>(settings.spawnRate);
                const auto baseSpawnPerFrame = static_cast<float>(settings.spawnPerFrame);
                const auto baseSpawnPerUnit = static_cast<float>(settings.spawnPerUnit);
                Builder builder(emitterGraph,materialized,emitter,stack,issues,effective,captured,emitterAttributes);
                builder.Seed("Emitter.SpawnRate",ValueType::Float,builder.Add(Operation::Constant,ValueType::Float,{}, {baseSpawnRate,0,0,0}));
                builder.Seed("Emitter.SpawnPerFrame",ValueType::Float,builder.Add(Operation::Constant,ValueType::Float,{}, {baseSpawnPerFrame,0,0,0}));
                builder.Seed("Emitter.SpawnPerUnit",ValueType::Float,builder.Add(Operation::Constant,ValueType::Float,{}, {baseSpawnPerUnit,0,0,0}));
                for (const auto& module : stack.modules) if (module.enabled)
                {
                    const auto fail = [&](std::string message) { issues.push_back({std::move(message),settings.id,stack.id,module.id}); };
                    const auto* definition = FindModule(module.script);
                    const ScriptVersion* version = nullptr;
                    if (!module.scriptAsset.Empty())
                        if (const auto* asset = effective.Script(module.scriptAsset.asset))
                            version = SelectVersion(*asset,module.scriptAsset.version);
                    std::vector<std::string> inputNames;
                    if (definition) for (const auto& input : definition->inputs) inputNames.push_back(input.name);
                    else if (version) for (const auto& input : version->inputs) inputNames.push_back(input.name);
                    else { fail("Unknown or incompatible emitter module"); continue; }
                    const uint32_t capabilities = version ? version->capabilities :
                        (definition ? definition->capabilities : uint32_t{});
                    // Literal bursts are constant-folded into the scheduler's
                    // burst list on every target; dynamic inputs stay on the
                    // EmitterUpdate graph and drive Emitter.SpawnCount.
                    if (capabilities & ModuleCapability_SpawnBurst)
                    {
                        bool literal = true;
                        for (const auto& name : inputNames)
                            if (!module.inputs.contains(name) || module.inputs.at(name).mode != InputMode::Literal) literal = false;
                        if (literal)
                        {
                            const auto value = [&](const std::string& name) { return module.inputs.at(name).value[0]; };
                            const auto count = static_cast<double>(value("Count"));
                            if (value("Time") < 0 || count < 0 || count > 10000000 || std::floor(count) != count) fail("Invalid burst time or integer count");
                            else settings.bursts.push_back({value("Time"),static_cast<uint32_t>(count)});
                            continue;
                        }
                    }
                    if (capabilities & ModuleCapability_CompleteIfUnused)
                        for (const auto& name : inputNames)
                            if (!module.inputs.contains(name) || module.inputs.at(name).mode != InputMode::Literal)
                                fail("Emitter completion inputs currently require literal values");
                    builder.Lower(module);
                }
                builder.Finish();
                settings.emitterAttributes = emitterAttributes;
                if (stack.stage == Stage::EmitterUpdate)
                {
                    std::map<std::string,float> outputs; std::string evaluateError;
                    if (!EvaluateEmitterGraph(emitterGraph,outputs,evaluateError))
                        issues.push_back({evaluateError,settings.id,stack.id});
                    else
                    {
                        const auto get = [&](const char* name,float& value)
                            {
                                const auto it = outputs.find(name);
                                if (it == outputs.end()) return false;
                                value = it->second; return true;
                            };
                            float value = 0;
                            if (get("Emitter.SpawnRate",value))
                            { if (!std::isfinite(value) || value < 0) issues.push_back({"Spawn rate must be nonnegative",settings.id,stack.id}); else settings.spawnRate = value; }
                            if (get("Emitter.SpawnPerFrame",value))
                            { if (!std::isfinite(value) || value < 0) issues.push_back({"Spawn Per Frame count must be nonnegative",settings.id,stack.id}); else settings.spawnPerFrame = static_cast<uint32_t>(value); }
                            if (get("Emitter.SpawnPerUnit",value))
                            { if (!std::isfinite(value) || value < 0 || value > 100000) issues.push_back({"Spawn Per Unit density must be between 0 and 100000",settings.id,stack.id}); else settings.spawnPerUnit = value; }
                            if (get("Emitter.Duration",value))
                            { if (!std::isfinite(value) || value <= 0) issues.push_back({"Emitter duration must be positive",settings.id,stack.id}); else settings.duration = value; }
                            if (get("Emitter.Loop",value)) settings.looping = value != 0;
                            if (get("Emitter.UnusedGracePeriod",value))
                            {
                                if (!std::isfinite(value) || value < 0 || value > 3600) issues.push_back({"Unused grace period must be between 0 and 3600 seconds",settings.id,stack.id});
                                else { lowered.completion.enabled = true; lowered.completion.unusedGracePeriod = value; }
                            }
                        }
                    }
                }
            else if (stack.stage == Stage::Event)
            {
                auto& eventHandler = settings.eventHandlers.emplace_back();
                eventHandler.id = stack.id;
                eventHandler.name = stack.name;
                auto& eventGraph = eventHandler.graph;
                eventGraph.id = stack.id;
                eventGraph.stage = ScriptStage::Event;
                eventGraph.nodes.clear(); eventGraph.nextNodeId = 1;
                std::vector<Attribute> eventAttributes = settings.emitterAttributes;
                const auto ensureEventAttribute = [&](const char* name,ValueType type,const Value& defaultValue)
                {
                    if (std::none_of(eventAttributes.begin(),eventAttributes.end(),
                        [&](const Attribute& attribute) { return attribute.name == name; }))
                        eventAttributes.push_back({name,type,defaultValue});
                };
                ensureEventAttribute("Emitter.GenerateSpawn",ValueType::Float,{}); ensureEventAttribute("Emitter.GenerateDeath",ValueType::Float,{});
                ensureEventAttribute("Emitter.GenerateCollision",ValueType::Float,{}); ensureEventAttribute("Emitter.ReceiveKind",ValueType::Float,{});
                ensureEventAttribute("Emitter.ReceiveSpawnCount",ValueType::Float,{1,0,0,0}); ensureEventAttribute("Emitter.ReceiveRandomAge",ValueType::Float,{});
                ensureEventAttribute("Emitter.ReceiveSourceIndex",ValueType::Float,{-1,0,0,0});
                ensureEventAttribute("Emitter.ReceiveExecutionMode",ValueType::Float,{});
                ensureEventAttribute("Emitter.ReceiveMaxEvents",ValueType::Float,{});
                ensureEventAttribute("Emitter.ReceivePosition",ValueType::Float3,{});
                ensureEventAttribute("Emitter.ReceiveVelocity",ValueType::Float3,{});
                ensureEventAttribute("Emitter.ReceiveNormal",ValueType::Float3,{});
                // The event payload columns exist for every target: a CPU
                // receiver writes them from the event program, a GPU receiver
                // writes them from the payload program in the spawn kernel.
                const auto ensureParticleAttribute = [&](const char* name,ValueType type)
                {
                    if (std::none_of(settings.attributes.begin(),settings.attributes.end(),
                        [&](const Attribute& attribute) { return attribute.name == name; }))
                        settings.attributes.push_back({name,type,{}});
                };
                ensureParticleAttribute("Particles.EventPosition",ValueType::Float3);
                ensureParticleAttribute("Particles.EventVelocity",ValueType::Float3);
                ensureParticleAttribute("Particles.EventNormal",ValueType::Float3);
                ensureParticleAttribute("Particles.EventKind",ValueType::Float);
                ensureParticleAttribute("Particles.EventParticleId",ValueType::Float);
                ensureParticleAttribute("Particles.EventSourceIndex",ValueType::Float);
                Builder builder(eventGraph,materialized,emitter,stack,issues,effective,captured,eventAttributes);
                for (const auto& module : stack.modules) if (module.enabled)
                {
                    const auto fail = [&](std::string message) { issues.push_back({std::move(message),settings.id,stack.id,module.id}); };
                    const auto* definition = FindModule(module.script);
                    const ScriptVersion* version = nullptr;
                    if (!module.scriptAsset.Empty())
                        if (const auto* asset = effective.Script(module.scriptAsset.asset))
                            version = SelectVersion(*asset,module.scriptAsset.version);
                    if (!definition && !version) { fail("Unknown or incompatible event module"); continue; }
                    builder.Lower(module);
                }
                builder.Finish();
                settings.emitterAttributes = eventAttributes;
                // Event modules may write particle attributes directly. Those
                // writes are compiled into the per-particle event program and
                // must exist in the receiving particle layout.
                for (const auto& node : eventGraph.nodes)
                    if (node.operation == Operation::WriteAttribute && node.name.starts_with("Particles."))
                    {
                        const auto existing = std::find_if(settings.attributes.begin(),settings.attributes.end(),
                            [&](const Attribute& attribute) { return attribute.name == node.name; });
                        if (existing == settings.attributes.end())
                            settings.attributes.push_back({node.name,node.type,{}});
                        else if (existing->type != node.type)
                            issues.push_back({"Event module writes an incompatible particle attribute: "+node.name,settings.id,stack.id});
                    }
                // Only outputs that actually consume Emitter.Event* payload
                // values are runtime-dynamic. Position/velocity forwarding does
                // not prevent the scalar receiver policy from being baked.
                std::set<NodeId> eventDependencyVisiting;
                std::function<bool(NodeId)> dependsOnEventPayload = [&](NodeId id) -> bool
                {
                    const auto* node = FindNode(eventGraph,id);
                    if (!node) return false;
                    if (node->operation == Operation::Parameter && node->name.starts_with("Emitter.Event")) return true;
                    if (!eventDependencyVisiting.insert(id).second) return false;
                    for (const auto input : node->inputs)
                        if (dependsOnEventPayload(input)) { eventDependencyVisiting.erase(id); return true; }
                    eventDependencyVisiting.erase(id);
                    return false;
                };
                const auto outputDependsOnEventPayload = [&](std::string_view name)
                {
                    for (const auto& node : eventGraph.nodes)
                        if (node.operation == Operation::WriteAttribute && node.name == name)
                            for (const auto input : node.inputs)
                                if (dependsOnEventPayload(input)) return true;
                    return false;
                };
                const bool dynamicGeneration =
                    outputDependsOnEventPayload("Emitter.GenerateSpawn") ||
                    outputDependsOnEventPayload("Emitter.GenerateDeath") ||
                    outputDependsOnEventPayload("Emitter.GenerateCollision");
                const bool dynamicReceiver =
                    outputDependsOnEventPayload("Emitter.ReceiveKind") ||
                    outputDependsOnEventPayload("Emitter.ReceiveSpawnCount") ||
                    outputDependsOnEventPayload("Emitter.ReceiveRandomAge") ||
                    outputDependsOnEventPayload("Emitter.ReceiveMaxEvents") ||
                    outputDependsOnEventPayload("Emitter.ReceiveExecutionMode");
                std::map<std::string,float> outputs; std::string evaluateError;
                if (!EvaluateEmitterGraph(eventGraph,outputs,evaluateError))
                    issues.push_back({evaluateError,settings.id,stack.id});
                else
                {
                    const auto get = [&](const char* name,float& value)
                    {
                        const auto it = outputs.find(name);
                        if (it == outputs.end()) return false;
                        value = it->second; return true;
                    };
                    float value = 0;
                    if (!dynamicGeneration &&
                        (get("Emitter.GenerateSpawn",value) || get("Emitter.GenerateDeath",value) ||
                         get("Emitter.GenerateCollision",value)))
                    {
                        if (!settings.hasEventGenerationPolicy)
                        { settings.hasEventGenerationPolicy = true; settings.eventGenerationMask = 0; }
                        const auto apply = [&](const char* name,const ParticleEventSelection selection)
                        {
                            float enabled = 0;
                            if (!get(name,enabled)) return;
                            if (enabled > 0.5f) settings.eventGenerationMask |= EventBit(selection);
                            else settings.eventGenerationMask &= ~EventBit(selection);
                        };
                        apply("Emitter.GenerateSpawn",ParticleEventSelection::Spawn);
                        apply("Emitter.GenerateDeath",ParticleEventSelection::Death);
                        apply("Emitter.GenerateCollision",ParticleEventSelection::Collision);
                    }
                    if (!dynamicReceiver)
                    {
                        if (get("Emitter.ReceiveKind",value))
                        {
                            const auto kind = static_cast<int32_t>(std::llround(value));
                            if (kind <= 0) eventHandler.receiver.eventKind = ParticleEventSelection::None;
                            else if (kind <= static_cast<int32_t>(ParticleEventSelection::Collision))
                                eventHandler.receiver.eventKind = static_cast<ParticleEventSelection>(kind);
                        }
                        if (get("Emitter.ReceiveSpawnCount",value))
                        {
                            const auto count = static_cast<double>(value);
                            if (count < 1 || count > 100000 || std::floor(count) != count)
                                issues.push_back({"Event spawn count must be a positive integer no greater than 100000",settings.id,stack.id});
                            else eventHandler.receiver.spawnCount = static_cast<uint32_t>(count);
                        }
                        if (get("Emitter.ReceiveRandomAge",value))
                        {
                            if (!std::isfinite(value) || value < 0 || value > 3600)
                                issues.push_back({"Event random age must be between 0 and 3600",settings.id,stack.id});
                            else eventHandler.receiver.randomAge = value;
                        }
                        if (get("Emitter.ReceiveMaxEvents",value))
                        {
                            const auto count = static_cast<double>(value);
                            if (!std::isfinite(value) || count < 0 || count > 1000000 || std::floor(count) != count)
                                issues.push_back({"Event max events per frame must be a nonnegative integer no greater than 1000000",settings.id,stack.id});
                            else eventHandler.receiver.maxEventsPerFrame = static_cast<uint32_t>(count);
                        }
                        if (get("Emitter.ReceiveExecutionMode",value))
                        {
                            const auto mode = static_cast<int32_t>(std::llround(value));
                            if (mode < 0 || mode > static_cast<int32_t>(EventExecutionMode::SingleParticle))
                                issues.push_back({"Event execution mode must be 0 (Spawned Particles), 1 (Every Particle) or 2 (Single Particle)",settings.id,stack.id});
                            else eventHandler.receiver.executionMode = static_cast<EventExecutionMode>(mode);
                        }
                        if (get("Emitter.ReceiveSourceIndex",value))
                        {
                            const auto index = static_cast<int32_t>(std::llround(value));
                            if (index < 0) eventHandler.receiver.sourceEmitter.clear();
                            else if (index >= static_cast<int32_t>(materialized.emitters.size()) || static_cast<size_t>(index) == emitterIndex)
                                issues.push_back({"Event source emitter index is invalid or refers to the receiving emitter",settings.id,stack.id});
                            else eventHandler.receiver.sourceEmitter = materialized.emitters[static_cast<size_t>(index)].settings.id;
                        }
                    }
                }
            }            else if (std::any_of(stack.modules.begin(),stack.modules.end(),[](const auto& module) { return module.enabled; }))
                issues.push_back({"This execution context is not implemented yet",settings.id,stack.id});
        }
        std::vector<Graph*> graphs{&settings.spawn,&settings.update,&settings.emitterSpawn,&settings.emitterUpdate,&settings.emitterCollision};
        for (auto& stage : settings.simulationStages) graphs.push_back(&stage.graph);
        for (auto& handler : settings.eventHandlers) graphs.push_back(&handler.graph);
        // Event Handler payload channels are runtime-reserved parameters. They
        // are published per delivered event by SystemInstance/CpuEmitter, so the
        // compiler must keep their declared types instead of treating them as
        // unknown user parameters.
        for (const auto& handler : settings.eventHandlers)
            for (const auto& node : handler.graph.nodes)
                if (node.operation == Operation::Parameter && node.name.starts_with("Emitter.Event") &&
                    std::none_of(lowered.parameters.begin(),lowered.parameters.end(),
                        [&](const ParameterDefinition& parameter) { return parameter.name == node.name; }))
                    lowered.parameters.push_back({node.name,node.type,{}});
        for (auto* graph : graphs)
        {
            std::vector<std::string> aggregateParameters;
            for(const auto& node:graph->nodes)
                if(node.operation==Operation::Parameter && !node.structType.empty() && node.name.starts_with("User."))aggregateParameters.push_back(node.name);
            Graph scalar;std::string error;
            if(!LowerAggregateGraph(*graph,scalar,error)){issues.push_back({error,settings.id,graph->id});continue;}
            for(const auto& node:scalar.nodes)
                if(node.operation==Operation::Parameter && std::any_of(aggregateParameters.begin(),aggregateParameters.end(),[&](const auto& name){return node.name.starts_with(name+".");}))
                {
                    const auto exists=std::find_if(lowered.parameters.begin(),lowered.parameters.end(),[&](const auto& value){return value.name==node.name;});
                    if(exists==lowered.parameters.end())lowered.parameters.push_back({node.name,node.type,node.value});
                }
            *graph=std::move(scalar);
        }
        for (auto* graph : graphs)
            for (auto& node : graph->nodes) if (node.operation == Operation::Parameter)
            {
                // Emitter-stage reserved values are published by the scheduler
                // each tick instead of coming from user parameters.
                if (node.name == "Emitter.Age") { node.value = {}; continue; }
                // Look in the lowered set as well: camera-relative modules
                // reference reserved Camera.* parameters that the host
                // publishes at runtime and that only exist after lowering.
                const auto parameter = std::find_if(lowered.parameters.begin(),lowered.parameters.end(),
                    [&](const ParameterDefinition& value) { return value.name == node.name; });
                if (parameter == lowered.parameters.end() || parameter->type != node.type)
                    issues.push_back({"Unknown or incompatible user parameter: "+node.name,settings.id,graph->id});
                else node.value = parameter->value;
            }
        std::stable_sort(settings.bursts.begin(),settings.bursts.end(),[](const Burst& a,const Burst& b) { return a.time < b.time; });
        lowered.emitters.push_back(std::move(settings));
    }
    bool typed = std::any_of(lowered.parameters.begin(),lowered.parameters.end(),[](const auto& p) { return IsIntegralType(p.type); });
    for (const auto& emitter : lowered.emitters)
    {
        for (const auto& a : emitter.attributes) typed |= IsIntegralType(a.type);
        const auto check = [&](const Graph& graph) { for (const auto& n : graph.nodes) typed |= IsIntegralType(n.type) || n.operation > Operation::StageIteration; };
        check(emitter.spawn); check(emitter.update); check(emitter.emitterSpawn); check(emitter.emitterUpdate); check(emitter.emitterCollision);
        for (const auto& handler : emitter.eventHandlers) check(handler.graph);
        for (const auto& stage : emitter.simulationStages) { check(stage.graph); typed |= stage.bindings.typed; }
    }
    // Float-only systems drop the declared parameters so their program identity
    // matches the pre-typed format. The reserved camera parameters must survive:
    // they carry live per-frame values the host supplies, not baked constants.
    if (!typed)
        std::erase_if(lowered.parameters,[](const ParameterDefinition& parameter)
        { return !parameter.name.starts_with("Camera.") && !parameter.name.starts_with("Engine.") &&
            !parameter.name.starts_with("System.") && !parameter.name.starts_with("Emitter.Event"); });
    if (!issues.empty()) return false;
    CompileCacheKey cacheKey;
    if (cache)
    {
        std::vector<SourceDependency> cacheDependencies;
        cacheDependencies.reserve(captured.size());
        for (const auto& [id,value] : captured) cacheDependencies.push_back(value);
        cacheKey = MakeCompileCacheKey(lowered,std::span<const uint32_t>{},HashDependencies(cacheDependencies));
        if (const auto cached = cache->Get(cacheKey))
        {
            dependencies.clear(); for (auto& [id,value] : captured) dependencies.push_back(std::move(value));
            if (resolved) *resolved = std::move(materialized);
            output = *cached;
            return true;
        }
    }
    CompiledSystem candidate; std::vector<Diagnostic> diagnostics;
    if (!CompileSystem(lowered,candidate,diagnostics))
    {
        for (const auto& diagnostic : diagnostics)
        {
            Issue issue{diagnostic.message,{},diagnostic.graphId};
            for (const auto& emitter : materialized.emitters)
                if (std::any_of(emitter.stacks.begin(),emitter.stacks.end(),[&](const auto& stack) { return stack.id == diagnostic.graphId; })) issue.emitter = emitter.settings.id;
            issues.push_back(std::move(issue));
        }
        return false;
    }
    if (cache) cache->Put(cacheKey,std::make_shared<CompiledSystem>(candidate));
    dependencies.clear(); for (auto& [id,value] : captured) dependencies.push_back(std::move(value));
    if (resolved) *resolved = std::move(materialized);
    output = std::move(candidate); return true;
}
bool BuildModuleGraph(const Module& module,Graph& graph,std::string& error,Stage stage)
{
    const auto* definition = FindModule(module.script);
    if (!definition) { error = "Unknown module: "+module.script; return false; }
    // A synthetic stack of the requested stage gives the lowering the same
    // context it sees inside a real emitter. Stage-dependent modules are
    // captured once per stage.
    System system; system.id = Guid::New().ToString(); system.name = "Module Graph";
    const auto emitter = CreateEmitter("Module Graph");
    Stack stack; stack.id = Guid::New().ToString(); stack.stage = stage; stack.name = StageName(stack.stage);
    std::vector<Issue> issues;
    SourceLibrary library;
    std::map<std::string,SourceDependency> dependencies;
    graph = {}; graph.id = Guid::New().ToString();
    graph.stage = stage == Stage::ParticleSpawn ? ScriptStage::ParticleSpawn :
        stage == Stage::ParticleUpdate ? ScriptStage::ParticleUpdate :
        stage == Stage::EmitterSpawn ? ScriptStage::EmitterSpawn :
        stage == Stage::EmitterUpdate ? ScriptStage::EmitterUpdate :
        stage == Stage::Event ? ScriptStage::Event : ScriptStage::SimulationStage;
    std::vector<Attribute> attributes;
    Builder builder(graph,system,emitter,stack,issues,library,dependencies,attributes);
    builder.BeginModuleCapture(module.script,definition->requiresModule);
    if (!builder.Lower(module)) { error = issues.empty() ? "Module lowering failed" : issues.front().message; return false; }
    builder.Finish();
    if (!issues.empty()) { error = issues.front().message; return false; }
    if (std::none_of(graph.nodes.begin(),graph.nodes.end(),[](const auto& node) { return node.operation == Operation::WriteAttribute; }))
    { error = "Module has no graph-expressible particle output"; return false; }
    error.clear(); return true;
}
}
