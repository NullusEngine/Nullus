#pragma once
#include "VfxGraph.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_set>

namespace NLS::VFX
{
enum class DataInterfaceMethod : uint32_t { Get, Length, Sample, IsValidIndex };
inline std::string DataInterfaceParameter(std::string_view name,size_t index)
{ return "DataInterface."+std::string(name)+".Data."+std::to_string(index); }
inline ValueType DataInterfaceStorageType(const DataInterfaceDefinition& binding)
{ return binding.kind == DataInterfaceKind::Curve ? ValueType::Float4 : binding.elementType; }
inline bool ValidateDataInterfaceData(const DataInterfaceDefinition& binding,std::span<const Value> data,std::string& error)
{
    if (binding.name.empty() || binding.name.size()>256 || binding.kind > DataInterfaceKind::Curve ||
        !IsValueType(binding.elementType) || data.size()>4096 ||
        (binding.kind == DataInterfaceKind::Curve && binding.elementType != ValueType::Float))
    { error = "Invalid data interface declaration: "+binding.name; return false; }
    for (size_t i=0;i<data.size();++i)
    {
        if (!ValidValue(data[i],DataInterfaceStorageType(binding)))
        { error = "Invalid data interface value: "+binding.name; return false; }
        if (binding.kind == DataInterfaceKind::Curve && i && !(data[i][0]>data[i-1][0]))
        { error = "Curve data interface key times must be strictly increasing: "+binding.name; return false; }
    }
    error.clear(); return true;
}
struct DataInterfaceCallBinding
{
    uint32_t index = 0;
    DataInterfaceMethod method = DataInterfaceMethod::Get;
    ValueType result = ValueType::Float;
};
inline bool ResolveDataInterfaceCall(std::span<const DataInterfaceDefinition> bindings,std::string_view name,
    std::span<const ValueType> inputs,ValueType output,DataInterfaceCallBinding& call,std::string& error)
{
    const auto separator = name.rfind('.');
    if (separator == std::string_view::npos) { error="Data interface call requires Binding.Function"; return false; }
    const auto found = std::find_if(bindings.begin(),bindings.end(),[&](const auto& binding) { return binding.name == name.substr(0,separator); });
    if (found == bindings.end()) { error="Unbound data interface: "+std::string(name.substr(0,separator)); return false; }
    call.index = static_cast<uint32_t>(found-bindings.begin());
    const auto method = name.substr(separator+1);
    if (method == "Length" && found->kind == DataInterfaceKind::Array)
    {
        call.method = DataInterfaceMethod::Length; call.result = ValueType::Int32;
        if (!inputs.empty()) { error="Array.Length takes no inputs"; return false; }
    }
    else if (method == "Get" && found->kind == DataInterfaceKind::Array)
    {
        call.method = DataInterfaceMethod::Get; call.result = found->elementType;
        if (inputs.size()!=1 || inputs[0]!=ValueType::Int32) { error="Array.Get requires one Int32 index"; return false; }
    }
    else if(method=="IsValidIndex" && found->kind==DataInterfaceKind::Array)
    {
        call.method=DataInterfaceMethod::IsValidIndex;call.result=ValueType::Bool;
        if(inputs.size()!=1 || inputs[0]!=ValueType::Int32){error="Array.IsValidIndex requires one Int32 index";return false;}
    }
    else if ((method == "Sample" || (method == "SampleCurve" && found->kind==DataInterfaceKind::Curve)) && IsFloatType(found->elementType))
    {
        call.method = DataInterfaceMethod::Sample; call.result = found->elementType;
        if (inputs.size()!=1 || inputs[0]!=ValueType::Float) { error="Data interface Sample requires one Float coordinate"; return false; }
    }
    else { error="Unknown or unsupported data interface function: "+std::string(name); return false; }
    if (output!=call.result) { error="Data interface result type mismatch: "+std::string(name); return false; }
    error.clear(); return true;
}
template<class Read> inline Value EvaluateDataInterface(const DataInterfaceDefinition& binding,
    DataInterfaceMethod method,std::span<const Value> inputs,Read&& read)
{
    const auto count=static_cast<uint32_t>(binding.data.size());
    if (method == DataInterfaceMethod::Length) return IntValue(static_cast<int32_t>(count));
    if(method==DataInterfaceMethod::IsValidIndex)
    {const auto index=ReadInt(inputs[0]);return BoolValue(index>=0 && static_cast<uint32_t>(index)<count);}
    if (count==0) return {};
    if (method == DataInterfaceMethod::Get)
    {
        const auto index=ReadInt(inputs[0]);
        // Niagara VMGetValue and NiagaraDataInterfaceArrayTemplate.ush clamp
        // non-empty arrays; an empty array returns the element default.
        return read(static_cast<uint32_t>(std::clamp(index,0,static_cast<int32_t>(count-1))));
    }
    const auto coordinate=inputs[0][0]; if (!std::isfinite(coordinate)) return {};
    uint32_t lo=0,hi=0; float alpha=0;
    if (binding.kind == DataInterfaceKind::Curve)
    {
        const auto first=read(0),last=read(count-1);
        if (coordinate<=first[0]) return {first[1],0,0,0};
        if (coordinate>=last[0]) return {last[1],0,0,0};
        for (hi=1;hi<count;++hi) if (coordinate<read(hi)[0]) break;
        lo=hi-1; const auto a=read(lo),b=read(hi);
        alpha=(coordinate-a[0])/(b[0]-a[0]);
        return {a[1]+(b[1]-a[1])*alpha,0,0,0};
    }
    const auto position=std::clamp(coordinate,0.0f,1.0f)*static_cast<float>(count-1);
    lo=static_cast<uint32_t>(position);hi=(std::min)(lo+1,count-1);alpha=position-static_cast<float>(lo);
    const auto a=read(lo),b=read(hi);Value result{};
    for (uint32_t c=0;c<ComponentCount(binding.elementType);++c) result[c]=a[c]+(b[c]-a[c])*alpha;
    return result;
}
inline bool ValidateGraphDeclarations(const Graph& graph,std::string& error)
{
    if (graph.structs.size()>64 || graph.dataInterfaces.size()>64) { error="Graph type/interface declaration limit exceeded"; return false; }
    std::unordered_map<std::string,const StructDefinition*> structs;
    for (const auto& type : graph.structs)
        if (type.name.empty() || type.name.size()>256 || type.fields.empty() || type.fields.size()>64 || !structs.emplace(type.name,&type).second)
        { error="Invalid or duplicate struct declaration: "+type.name; return false; }
    std::unordered_set<std::string> visiting,done;
    std::function<bool(const StructDefinition&,uint32_t)> validate=[&](const StructDefinition& type,uint32_t depth)
    {
        if (done.contains(type.name)) return true;
        if (depth>32 || !visiting.insert(type.name).second) { error="Recursive struct or type depth exceeded: "+type.name; return false; }
        std::unordered_set<std::string> fields;
        for (const auto& field : type.fields)
        {
            if (field.name.empty() || field.name.find('.')!=std::string::npos || !fields.insert(field.name).second || !ValidValue(field.defaultValue,field.type))
            { error="Invalid or duplicate struct field: "+field.name; return false; }
            if (!field.structType.empty())
            {
                const auto nested=structs.find(field.structType);
                if (nested==structs.end()) { error="Unknown nested struct type: "+field.structType; return false; }
                if (!validate(*nested->second,depth+1)) return false;
            }
        }
        visiting.erase(type.name);done.insert(type.name);return true;
    };
    for (const auto& type : graph.structs) if (!validate(type,0)) return false;
    std::unordered_set<std::string> names; size_t records=0;
    for (const auto& binding : graph.dataInterfaces)
    {
        if (!names.insert(binding.name).second || !ValidateDataInterfaceData(binding,binding.data,error))
        { if(error.empty()) error="Duplicate data interface: "+binding.name; return false; }
        records+=binding.data.size();
    }
    if (records>16384) { error="Data interface uniform record limit exceeded"; return false; }
    error.clear();return true;
}
// Module expansion merges declarations by identity; conflicting definitions do
// not silently choose one module's data or field layout.
inline bool MergeGraphDeclarations(Graph& target,const Graph& source,std::string& error)
{
    for (const auto& type : source.structs)
    {
        const auto found=std::find_if(target.structs.begin(),target.structs.end(),[&](const auto& t){return t.name==type.name;});
        if(found==target.structs.end()) target.structs.push_back(type);
        else if(*found!=type) {error="Conflicting struct declaration: "+type.name;return false;}
    }
    for (const auto& binding : source.dataInterfaces)
    {
        const auto found=std::find_if(target.dataInterfaces.begin(),target.dataInterfaces.end(),[&](const auto& b){return b.name==binding.name;});
        if(found==target.dataInterfaces.end()) target.dataInterfaces.push_back(binding);
        else if(*found!=binding) {error="Conflicting data interface binding: "+binding.name;return false;}
    }
    return ValidateGraphDeclarations(target,error);
}
}
