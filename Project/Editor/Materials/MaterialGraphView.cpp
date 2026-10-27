#include "MaterialGraphView.h"
#include "Inspector/ReflectedEditHost.h"
#include "Math/ExternalReflection.h"
#include <algorithm>
#include <set>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <UI/ImGuiExtensions/ImGuiNodeEditor/imgui_node_editor.h>

namespace NLS::Editor::Materials
{
namespace
{
namespace ed = ax::NodeEditor;
using namespace MaterialCompiler;
using Type = MaterialGraphType;
struct Pin
{
    uint64_t node = 0;
    std::string name;
    uint32_t output = 0;
    bool source = false;
};
std::vector<std::string> Inputs(const MaterialGraphNode& node)
{
    const auto& op = node.operation;
    if (op == "CustomHLSL")
    {
        std::vector<std::string> result;
        for (const auto& p : node.customInputs)
            result.push_back(p.name);
        return result;
    }
    if (op == "Function")
    {
        std::vector<std::string> result;
        for (const auto& [name, link] : node.inputs)
            result.push_back(name);
        return result;
    }
    if (op == "StaticSwitch")
        return {"True", "False"};
    if (op == "SampleTexture")
        return {"Texture", "Sampler", "UV", "LOD"};
    if (op == "Add" || op == "Subtract" || op == "Multiply" || op == "Divide" || op == "Dot" || op == "Cross" || op == "Append")
        return {"A", "B"};
    if (op == "Lerp")
        return {"A", "B", "T"};
    if (op == "Clamp")
        return {"Value", "Min", "Max"};
    if (op == "Power")
        return {"Base", "Exponent"};
    if (op == "Fresnel")
        return {"Exponent"};
    if (op == "Noise")
        return {"Position"};
    if (op == "NormalMap")
        return {"Sample"};
    if (op == "Swizzle" || op == "OneMinus" || op == "Saturate" || op == "Sine" || op == "Cosine" || op == "Frac" || op == "Abs" || op == "Normalize" || op == "Length" || op == "DDX" || op == "DDY")
        return {"Value"};
    return {};
}
bool WouldCycle(const MaterialGraphAsset& graph, uint64_t source, uint64_t destination)
{
    std::set<uint64_t> seen;
    const auto visit = [&](auto&& self, uint64_t id) -> bool
    {
        if (id == destination)
            return true;
        if (!seen.insert(id).second)
            return false;
        const auto* node = FindMaterialGraphNode(graph, id);
        if (!node)
            return false;
        for (const auto& [pin, link] : node->inputs)
            if (link.node && self(self, link.node))
                return true;
        return false;
    };
    return visit(visit, source);
}
template<class T>
bool DrawGraphField(Properties::PropertyPanel& editor, Inspector::ReflectedPropertyEdit& aggregate,
                    const char* label, T& value, const Inspector::ReflectedPropertyOptions& options = {})
{
    const auto edit = editor.Draw(std::to_string(ImGui::GetID(label)), label, value, options);
    aggregate.changed |= edit.changed;
    aggregate.completed |= edit.completed;
    aggregate.canceled |= edit.canceled;
    aggregate.paths.insert(aggregate.paths.end(), edit.paths.begin(), edit.paths.end());
    return edit.changed;
}
bool TypeControl(Properties::PropertyPanel& editor, Inspector::ReflectedPropertyEdit& aggregate, const char* label, Type& type, bool resources = true)
{
    const char* names[] = {"Float", "Float2", "Float3", "Float4", "Int", "Static Bool", "Texture2D", "TextureCube", "Sampler"};
    int32_t value = static_cast<int32_t>(type);
    Inspector::ReflectedPropertyOptions options;
    for (int32_t index = 0; index < (resources ? 9 : 6); ++index)
    {
        Inspector::InspectorPropertyChoice choice{std::to_string(index), names[index]};
        choice.typedValue = index;
        options.choices.push_back(std::move(choice));
    }
    if (!DrawGraphField(editor, aggregate, label, value, options))
        return false;
    type = static_cast<Type>(value);
    return true;
}
class GraphPropertyEditHost final : public Inspector::IReflectedEditHost
{
public:
    GraphPropertyEditHost(SourceTools::MaterialSourceDocument& document, const MaterialGraphAsset& graph)
        : m_document(document), m_graph(graph) {}
    bool BeginPropertyEdit(std::string_view label) override { return m_document.BeginEdit(std::string(label)); }
    bool PreviewPropertyEdit() override { return m_document.PreviewEdit(SerializeMaterialGraph(m_graph)); }
    bool CommitPropertyEdit() override { return m_document.EndEdit(); }
    bool CancelPropertyEdit() override { return m_document.EndEdit(false); }
    bool IsPropertyEditOpen() const override { return m_document.IsEditing(); }

private:
    SourceTools::MaterialSourceDocument& m_document;
    const MaterialGraphAsset& m_graph;
};
void Ports(Properties::PropertyPanel& editor, Inspector::ReflectedPropertyEdit& aggregate, const char* label, std::vector<MaterialGraphPort>& ports, bool& changed)
{
    if (!ImGui::TreeNode(label))
        return;
    for (size_t i = 0; i < ports.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        changed |= DrawGraphField(editor, aggregate, "Name", ports[i].name);
        changed |= TypeControl(editor, aggregate, "Type", ports[i].type);
        if (ImGui::SmallButton("Remove"))
        {
            ports.erase(ports.begin() + i);
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add port"))
    {
        uint64_t id = 1;
        for (const auto& port : ports)
            id = std::max(id, port.id + 1);
        ports.push_back({id, "Port" + std::to_string(id), Type::Float});
        changed = true;
    }
    ImGui::TreePop();
}
} // namespace

MaterialGraphView::MaterialGraphView()
{
    ed::Config config;
    config.SettingsFile = nullptr;
    m_context = ed::CreateEditor(&config);
}
MaterialGraphView::~MaterialGraphView()
{
    if (m_context)
        ed::DestroyEditor(m_context);
}

void MaterialGraphView::DrawSettings(MaterialGraphAsset& graph, bool& changed)
{
    if (ImGui::CollapsingHeader("Graph settings"))
    {
        changed |= DrawGraphField(m_properties, m_propertyEdit, "Name", graph.name);
        int domain = static_cast<int>(graph.domain), shading = static_cast<int>(graph.shading), blend = static_cast<int>(graph.blend);
        const auto choice = [&](const char* label, int& value, std::initializer_list<const char*> names)
        {Inspector::ReflectedPropertyOptions options;int32_t index=0;for(const auto* name:names){Inspector::InspectorPropertyChoice item{std::to_string(index),name};item.typedValue=index++;options.choices.push_back(std::move(item));}return DrawGraphField(m_properties,m_propertyEdit,label,value,options); };
        if (choice("Domain", domain, {"Surface", "Particle Sprite"}))
        {
            graph.domain = static_cast<MaterialGraphDomain>(domain);
            if (graph.domain == MaterialGraphDomain::ParticleSprite)
                graph.shading = MaterialGraphShading::Unlit;
            changed = true;
        }
        if (choice("Shading", shading, {"PBR", "Unlit"}))
        {
            graph.shading = static_cast<MaterialGraphShading>(shading);
            changed = true;
        }
        if (choice("Blend", blend, {"Opaque", "Masked", "Alpha", "Premultiplied", "Additive"}))
        {
            graph.blend = static_cast<MaterialGraphBlend>(blend);
            changed = true;
        }
        changed |= DrawGraphField(m_properties, m_propertyEdit, "Double sided", graph.doubleSided);
        Inspector::ReflectedPropertyOptions displacement;
        displacement.metadata.emplace();
        displacement.metadata->min = 0;
        displacement.metadata->max = 100000;
        changed |= DrawGraphField(m_properties, m_propertyEdit, "Maximum world displacement", graph.maximumWorldDisplacement, displacement);
        if (graph.function)
        {
            Ports(m_properties, m_propertyEdit, "Function inputs", graph.functionInputs, changed);
            Ports(m_properties, m_propertyEdit, "Function outputs", graph.functionOutputs, changed);
        }
    }
    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (size_t i = 0; i < graph.parameters.size(); ++i)
        {
            auto& p = graph.parameters[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode(p.name.c_str()))
            {
                changed |= DrawGraphField(m_properties, m_propertyEdit, "Label", p.name);
                changed |= DrawGraphField(m_properties, m_propertyEdit, "Group", p.group);
                auto type = p.type;
                if (TypeControl(m_properties, m_propertyEdit, "Value type", type) && type != Type::Sampler)
                {
                    p.type = type;
                    p.isStatic = type == Type::Bool;
                    if (type == Type::Bool)
                        p.value = false;
                    else if (type == Type::Int)
                        p.value = int32_t{};
                    else if (type == Type::Texture2D || type == Type::TextureCube)
                        p.value = std::string{};
                    else if (type == Type::Float)
                        p.value = 0.f;
                    else
                        p.value = std::array<float, 4>{};
                    changed = true;
                }
                std::visit([&](auto& v)
                           {
                    using T=std::decay_t<decltype(v)>;
                    if constexpr(std::is_same_v<T,std::array<float,4>>)
                    {
                        changed|=DrawGraphField(m_properties,m_propertyEdit,"Color",p.color);
                        if(p.color){Maths::Color color(v[0],v[1],v[2],v[3]);changed|=DrawGraphField(m_properties,m_propertyEdit,"Default",color);v={color.r,color.g,color.b,color.a};}
                        else{Maths::Vector4 vector(v[0],v[1],v[2],v[3]);changed|=DrawGraphField(m_properties,m_propertyEdit,"Default",vector);v={vector.x,vector.y,vector.z,vector.w};}
                    }
                    else if constexpr(std::is_same_v<T,std::array<float,16>>)
                    {
                        for(size_t row=0;row<4;++row){Maths::Vector4 vector(v[row*4],v[row*4+1],v[row*4+2],v[row*4+3]);ImGui::PushID(static_cast<int>(row));changed|=DrawGraphField(m_properties,m_propertyEdit,"Default",vector);ImGui::PopID();v[row*4]=vector.x;v[row*4+1]=vector.y;v[row*4+2]=vector.z;v[row*4+3]=vector.w;}
                    }
                    else changed|=DrawGraphField(m_properties,m_propertyEdit,"Default",v); },
                           p.value);
                bool range = p.range.has_value();
                if (DrawGraphField(m_properties, m_propertyEdit, "Range", range))
                {
                    p.range = range ? std::optional{std::array<float, 2>{0, 1}} : std::nullopt;
                    changed = true;
                }
                if (p.range)
                {
                    Maths::Vector2 bounds((*p.range)[0], (*p.range)[1]);
                    changed |= DrawGraphField(m_properties, m_propertyEdit, "Min / Max", bounds);
                    *p.range = {bounds.x, bounds.y};
                }
                if (ImGui::Button("Create parameter node"))
                {
                    const auto id = AddMaterialGraphNode(graph, p.isStatic ? "StaticSwitch" : "Parameter", p.isStatic ? Type::Float : p.type);
                    graph.nodes.back().parameter = p.id;
                    m_selected = id;
                    changed = true;
                }
                if (ImGui::Button("Remove parameter"))
                {
                    graph.parameters.erase(graph.parameters.begin() + i);
                    changed = true;
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (ImGui::Button("Add parameter"))
        {
            const auto name = "Parameter " + std::to_string(graph.parameters.size() + 1);
            uint64_t id = Render::Resources::MakeMaterialParameterId(graph.id, name);
            while (std::any_of(graph.parameters.begin(), graph.parameters.end(), [&](const auto& p)
                               { return p.id == id; }))
                ++id;
            graph.parameters.push_back({id, name});
            changed = true;
        }
    }
}

void MaterialGraphView::DrawNodeProperties(MaterialGraphAsset& graph, bool& changed)
{
    auto* node = FindMaterialGraphNode(graph, m_selected);
    if (!node)
        return;
    ImGui::PushID(std::to_string(node->id).c_str());
    ImGui::SeparatorText(node->operation.c_str());
    changed |= DrawGraphField(m_properties, m_propertyEdit, "Node label / swizzle", node->name);
    changed |= TypeControl(m_properties, m_propertyEdit, "Output type", node->type);
    if (node->operation == "Constant" || node->operation == "DynamicParameter")
    {
        Maths::Vector4 value(node->value[0], node->value[1], node->value[2], node->value[3]);
        changed |= DrawGraphField(m_properties, m_propertyEdit, "Value", value);
        node->value = {value.x, value.y, value.z, value.w};
    }
    if (node->operation == "Parameter" || node->operation == "StaticSwitch")
        for (const auto& p : graph.parameters)
            if (ImGui::Selectable(p.name.c_str(), p.id == node->parameter))
            {
                node->parameter = p.id;
                changed = true;
            }
    if (node->operation == "FunctionInput")
        for (const auto& p : graph.functionInputs)
            if (ImGui::Selectable(p.name.c_str(), p.id == node->parameter))
            {
                node->parameter = p.id;
                node->type = p.type;
                changed = true;
            }
    if (node->operation == "CustomHLSL")
    {
        Ports(m_properties, m_propertyEdit, "Inputs", node->customInputs, changed);
        Ports(m_properties, m_propertyEdit, "Outputs", node->customOutputs, changed);
        Inspector::ReflectedPropertyOptions codeOptions;codeOptions.metadata.emplace();codeOptions.metadata->multiline=true;codeOptions.metadata->codeLanguage="hlsl";
        changed |= DrawGraphField(m_properties,m_propertyEdit,"HLSL",node->code,codeOptions);
        for (size_t i = 0; i < node->includes.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            changed |= DrawGraphField(m_properties, m_propertyEdit, "Include", node->includes[i]);
            ImGui::PopID();
        }
        if (ImGui::Button("Add include"))
        {
            node->includes.emplace_back();
            changed = true;
        }
    }
    if (node->operation == "Function")
    {
        changed |= DrawGraphField(m_properties, m_propertyEdit, "Function asset", node->function.path);
        Ports(m_properties, m_propertyEdit, "Call inputs", node->customInputs, changed);
        Ports(m_properties, m_propertyEdit, "Call outputs", node->customOutputs, changed);
        for (const auto& port : node->customInputs)
            node->inputs.try_emplace(port.name, MaterialGraphLink{});
    }
    Inspector::ReflectedPropertyOptions commentOptions;commentOptions.metadata.emplace();commentOptions.metadata->multiline=true;
    changed |= DrawGraphField(m_properties,m_propertyEdit,"Comment",graph.comments[node->id],commentOptions);
    ImGui::PopID();
}

void MaterialGraphView::Draw(SourceTools::MaterialSourceDocument& document)
{
    MaterialGraphAsset graph;
    if (m_workingGraph && m_sourceRevision == document.Revision())
        graph = *m_workingGraph;
    else if (!DeserializeMaterialGraph(document.Asset(), graph, m_error))
    {
        ImGui::TextWrapped("%s", m_error.c_str());
        return;
    }
    m_properties.SetLifetime(document.Lifetime());
    m_propertyEdit = {};
    ImGui::PushID(graph.id.c_str());
    bool changed = false;
    const float sidebar = std::min(320.f, ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::BeginChild("Graph properties", {sidebar, 0}, true);
    DrawSettings(graph, changed);
    DrawNodeProperties(graph, changed);
    if (!m_error.empty())
        ImGui::TextWrapped("%s", m_error.c_str());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Graph canvas", {0, 0});
    ed::SetCurrentEditor(m_context);
    if (m_revision != document.Revision())
    {
        for (const auto& node : graph.nodes)
            ed::SetNodePosition(ed::NodeId(node.id + 2), {node.position[0], node.position[1]});
        if (!m_revision)
            ed::SetNodePosition(ed::NodeId(1), {650, 150});
        m_revision = document.Revision();
    }
    std::map<uintptr_t, Pin> pins;
    std::map<std::pair<uint64_t, uint32_t>, ed::PinId> outputs;
    struct Edge
    {
        uint64_t node;
        std::string pin;
    };
    std::map<uintptr_t, Edge> edges;
    std::map<std::pair<uint64_t, std::string>, ed::PinId> inputs;
    uintptr_t serial = 1;
    const auto addPin = [&](Pin pin)
    {const ed::PinId id(serial++);pins[id.Get()]=pin;ed::BeginPin(id,pin.source?ed::PinKind::Output:ed::PinKind::Input);
        ImGui::TextUnformatted(pin.name.c_str());ed::EndPin();return id; };
    ed::Begin("MaterialGraph", ImGui::GetContentRegionAvail());
    for (const auto& node : graph.nodes)
    {
        ed::BeginNode(ed::NodeId(node.id + 2));
        ImGui::TextUnformatted(node.operation.c_str());
        if (!node.name.empty())
            ImGui::TextDisabled("%s", node.name.c_str());
        for (const auto& pin : Inputs(node))
            inputs[{node.id, pin}] = addPin({node.id, pin, 0, false});
        const auto count = std::max(size_t(1), node.customOutputs.size());
        for (uint32_t i = 0; i < count; ++i)
            outputs[{node.id, i}] = addPin({node.id, i < node.customOutputs.size() ? node.customOutputs[i].name : "Value", i, true});
        ed::EndNode();
    }
    ed::BeginNode(ed::NodeId(1));
    ImGui::TextUnformatted(graph.function ? "Function Output" : "Material Output");
    std::vector<std::string> rootPins;
    if (graph.function)
        for (const auto& port : graph.functionOutputs)
            rootPins.push_back(port.name);
    else
        rootPins = {"BaseColor", "Metallic", "Roughness", "AmbientOcclusion", "Normal", "Emissive", "Opacity", "AlphaCutoff", "WorldPositionOffset"};
    for (const auto& pin : rootPins)
        inputs[{0, pin}] = addPin({0, pin, 0, false});
    ed::EndNode();
    uintptr_t linkId = 1;
    const auto edge = [&](uint64_t node, const std::string& pin, const MaterialGraphLink& link)
    {
        if (!link.node)
            return;
        const auto source = outputs.find({link.node, link.output});
        const auto destination = inputs.find({node, pin});
        if (source == outputs.end() || destination == inputs.end())
            return;
        edges[linkId] = {node, pin};
        ed::Link(ed::LinkId(linkId++), source->second, destination->second);
    };
    for (const auto& node : graph.nodes)
        for (const auto& [pin, link] : node.inputs)
            edge(node.id, pin, link);
    for (const auto& [pin, link] : graph.outputs)
        edge(0, pin, link);
    if (ed::BeginCreate())
    {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && pins.contains(a.Get()) && pins.contains(b.Get()))
        {
            auto source = pins.at(a.Get()), destination = pins.at(b.Get());
            if (!source.source)
                std::swap(source, destination);
            if (source.source && !destination.source && (!destination.node || !WouldCycle(graph, source.node, destination.node)))
            {
                if (ed::AcceptNewItem())
                {
                    auto& links = destination.node ? FindMaterialGraphNode(graph, destination.node)->inputs : graph.outputs;
                    links[destination.name] = {source.node, source.output};
                    changed = true;
                }
            }
            else
                ed::RejectNewItem({0.9f, 0.2f, 0.2f, 1});
        }
    }
    ed::EndCreate();
    if (ed::BeginDelete())
    {
        ed::LinkId link;
        while (ed::QueryDeletedLink(&link))
            if (edges.contains(link.Get()) && ed::AcceptDeletedItem())
            {
                const auto& e = edges.at(link.Get());
                (e.node ? FindMaterialGraphNode(graph, e.node)->inputs : graph.outputs).erase(e.pin);
                changed = true;
            }
        ed::NodeId node;
        while (ed::QueryDeletedNode(&node))
        {
            if (node.Get() < 2)
            {
                ed::RejectDeletedItem();
                continue;
            }
            if (ed::AcceptDeletedItem())
            {
                const auto id = node.Get() - 2;
                std::erase_if(graph.nodes, [&](const auto& n)
                              { return n.id == id; });
                for (auto& n : graph.nodes)
                    std::erase_if(n.inputs, [&](const auto& p)
                                  { return p.second.node == id; });
                std::erase_if(graph.outputs, [&](const auto& p)
                              { return p.second.node == id; });
                graph.comments.erase(id);
                changed = true;
            }
        }
    }
    ed::EndDelete();
    if (ed::ShowBackgroundContextMenu())
    {
        ed::Suspend();
        ImGui::OpenPopup("Add Material Node");
        ed::Resume();
    }
    ed::Suspend();
    if (ImGui::BeginPopup("Add Material Node"))
    {
        const char* operations[] = {"Constant", "Parameter", "StaticSwitch", "UV", "WorldPosition", "WorldNormal", "ViewDirection", "Time", "Add", "Subtract", "Multiply", "Divide", "Lerp", "Clamp", "Power", "OneMinus", "Saturate", "Sine", "Cosine", "Frac", "Abs", "Normalize", "Length", "Dot", "Cross", "Fresnel", "Noise", "NormalMap", "Sampler", "SampleTexture", "Swizzle", "Append", "DDX", "DDY", "Function", "FunctionInput", "CustomHLSL", "ParticleColor", "ParticleBaseColor", "ParticleAge", "ParticleNormalizedAge", "ParticleVelocity", "DynamicParameter"};
        for (const auto* op : operations)
            if (ImGui::MenuItem(op))
            {
                m_selected = AddMaterialGraphNode(graph, op);
                auto& n = graph.nodes.back();
                const auto pos = ed::ScreenToCanvas(ImGui::GetMousePos());
                n.position = {pos.x, pos.y};
                if (n.operation == "CustomHLSL")
                    n.code = "return 0.0;";
                changed = true;
            }
        ImGui::EndPopup();
    }
    ed::Resume();
    ed::End();
    ed::NodeId selected;
    if (ed::GetSelectedNodes(&selected, 1) == 1 && selected.Get() >= 2)
        m_selected = selected.Get() - 2;
    bool moved = false;
    if (!changed)
        for (auto& node : graph.nodes)
        {
            const auto p = ed::GetNodePosition(ed::NodeId(node.id + 2));
            if (std::abs(p.x - node.position[0]) > 0.1f || std::abs(p.y - node.position[1]) > 0.1f)
            {
                node.position = {p.x, p.y};
                moved = true;
            }
        }
    ed::SetCurrentEditor(nullptr);
    ImGui::EndChild();
    ImGui::PopID();
    GraphPropertyEditHost host(document, graph);
    if (!Inspector::ApplyReflectedEdit(host, m_propertyEdit, "Edit material graph properties"))
        m_error = document.Error();
    if (m_propertyEdit.canceled)
    {
        m_workingGraph.reset();
        m_sourceRevision = 0;
        return;
    }
    if (changed && !m_propertyEdit.changed)
    {
        (void)document.EndEdit();
        if (!document.Apply(SerializeMaterialGraph(graph), "Edit material graph"))
            m_error = document.Error();
    }
    else if (moved)
    {
        if (document.BeginEdit("Move material nodes"))
            (void)document.PreviewEdit(SerializeMaterialGraph(graph));
    }
    (void)Inspector::FinishReflectedEdits(m_properties, host);
    m_workingGraph = std::move(graph);
    m_sourceRevision = document.Revision();
}
} // namespace NLS::Editor::Materials
