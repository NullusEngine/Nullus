#pragma once
#include "VfxDataInterface.h"
#include <map>

namespace NLS::VFX
{
// Aggregate values are compile-time field bundles. Scalar leaves remain SSA
// values, so the runtime keeps the existing particle SoA and exact Int32 bits.
inline bool LowerAggregateGraph(const Graph& source,Graph& output,std::string& error)
{
    if (source.nodes.size()>65536 || !ValidateGraphDeclarations(source,error)) { if(error.empty())error="Graph node limit exceeded";return false; }
    Graph result=source; result.nodes.clear();
    NodeId next=source.nextNodeId;
    std::unordered_map<NodeId,const Node*> nodes;
    for (const auto& node : source.nodes)
    {
        if (!node.id || node.id>=(uint64_t{1}<<60) || !nodes.emplace(node.id,&node).second) {error="Invalid or duplicate graph node ID";return false;}
        next=(std::max)(next,node.id+1);
    }
    struct Bundle {NodeId value=0;ValueType type=ValueType::Float;std::string aggregate;std::vector<Bundle> fields;};
    std::map<NodeId,Bundle> emitted;std::unordered_set<NodeId> visiting;
    const auto declaration=[&](std::string_view name)->const StructDefinition*
    {const auto i=std::find_if(source.structs.begin(),source.structs.end(),[&](const auto& d){return d.name==name;});return i==source.structs.end()?nullptr:&*i;};
    const auto append=[&](Node node)->NodeId {if(result.nodes.size()>=65536){error="Aggregate expansion exceeds 65536 nodes";return 0;}if(!node.id) node.id=next++;const auto id=node.id;result.nodes.push_back(std::move(node));return id;};
    std::function<bool(const Node&,std::string_view,const std::string&,Bundle&,uint32_t)> expand;
    expand=[&](const Node& node,std::string_view typeName,const std::string& path,Bundle& bundle,uint32_t depth)
    {
        const auto* type=declaration(typeName); if(!type || depth>32) {error="Unknown aggregate type: "+std::string(typeName);return false;}
        bundle.aggregate=type->name;
        for(const auto& field:type->fields)
        {
            Bundle child;const auto name=path+"."+field.name;
            if(!field.structType.empty()) {if(!expand(node,field.structType,name,child,depth+1))return false;}
            else {Node leaf=node;leaf.id=0;leaf.structType.clear();leaf.type=field.type;leaf.name=name;leaf.value=field.defaultValue;child.type=field.type;child.value=append(std::move(leaf));}
            bundle.fields.push_back(std::move(child));
        }
        return true;
    };
    std::function<bool(NodeId,Bundle&,uint32_t)> emit;
    emit=[&](NodeId id,Bundle& out,uint32_t depth)
    {
        if(const auto f=emitted.find(id);f!=emitted.end()){out=f->second;return true;}
        const auto found=nodes.find(id);
        if(found==nodes.end() || depth>512 || !visiting.insert(id).second){error="Missing node, dependency cycle or graph depth exceeded";return false;}
        const auto& node=*found->second;std::vector<Bundle> inputs;
        for(auto input:node.inputs){Bundle value;if(!emit(input,value,depth+1))return false;inputs.push_back(std::move(value));}
        if(node.operation==Operation::StructMake)
        {
            const auto* type=declaration(node.name);
            if(!type || inputs.size()!=type->fields.size()){error="StructMake has an unknown type or wrong field count: "+node.name;return false;}
            out.aggregate=type->name;out.fields=inputs;
            for(size_t f=0;f<inputs.size();++f)
                if(inputs[f].aggregate!=type->fields[f].structType || (inputs[f].aggregate.empty() && inputs[f].type!=type->fields[f].type))
                {error="StructMake field type mismatch: "+type->fields[f].name;return false;}
        }
        else if(node.operation==Operation::StructGet)
        {
            if(inputs.size()!=1 || inputs[0].aggregate.empty()){error="StructGet requires one struct input";return false;}
            const auto* type=declaration(inputs[0].aggregate);const auto field=std::find_if(type->fields.begin(),type->fields.end(),[&](const auto& f){return f.name==node.name;});
            if(field==type->fields.end()){error="Unknown struct field: "+node.name;return false;}
            out=inputs[0].fields[field-type->fields.begin()];
            if(out.aggregate.empty() && node.type!=out.type){error="StructGet result type mismatch: "+node.name;return false;}
        }
        else if(!node.structType.empty() && (node.operation==Operation::Parameter || node.operation==Operation::Attribute || node.operation==Operation::Constant))
        {
            if(!inputs.empty() || !expand(node,node.structType,node.name,out,0))return false;
        }
        else if(node.operation==Operation::Select && inputs.size()==3 && !inputs[1].aggregate.empty())
        {
            if(inputs[0].aggregate.size() || inputs[0].type!=ValueType::Bool || inputs[1].aggregate!=inputs[2].aggregate){error="Struct Select requires Bool and matching struct types";return false;}
            std::function<Bundle(const Bundle&,const Bundle&)> choose=[&](const Bundle& a,const Bundle& b)
            {
                Bundle value=a;
                if(!a.aggregate.empty()){for(size_t f=0;f<a.fields.size();++f)value.fields[f]=choose(a.fields[f],b.fields[f]);}
                else {Node selected;selected.operation=Operation::Select;selected.type=a.type;selected.inputs={inputs[0].value,a.value,b.value};value.value=append(std::move(selected));}
                return value;
            };out=choose(inputs[1],inputs[2]);
        }
        else if(node.operation==Operation::WriteAttribute && !node.structType.empty())
        {
            if(inputs.size()!=1 || inputs[0].aggregate!=node.structType){error="Struct output type mismatch";return false;}
            std::function<void(const Bundle&,const std::string&)> write=[&](const Bundle& value,const std::string& name)
            {
                if(!value.aggregate.empty()){const auto* type=declaration(value.aggregate);for(size_t f=0;f<value.fields.size();++f)write(value.fields[f],name+"."+type->fields[f].name);}
                else {Node leaf=node;leaf.id=0;leaf.structType.clear();leaf.type=value.type;leaf.name=name;leaf.inputs={value.value};append(std::move(leaf));}
            };write(inputs[0],node.name);
        }
        else
        {
            if(!ValidInputCount(node.operation,inputs.size()) || node.operation>Operation::DataInterfaceCall){error="Invalid graph operation or input count";return false;}
            Node scalar=node;scalar.inputs.clear();
            for(const auto& value:inputs){if(!value.aggregate.empty()){error="Operation requires a scalar/vector input; extract the struct field first";return false;}scalar.inputs.push_back(value.value);}
            out.type=node.type;out.value=append(std::move(scalar));
        }
        if(!error.empty())return false;
        visiting.erase(id);emitted[id]=out;return true;
    };
    for(const auto& node:source.nodes){Bundle value;if(!emit(node.id,value,0))return false;}
    result.nextNodeId=next;output=std::move(result);error.clear();return true;
}
}
