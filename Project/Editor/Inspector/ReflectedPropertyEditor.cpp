#include "ReflectedPropertyEditor.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <imgui.h>
#include "Inspector/InspectorProjectionBuilder.h"
#include "Inspector/InspectorPropertyEditor.h"
#include "Inspector/PropertyTypeTraits.h"
#include "Properties/PropertyDrawerRegistry.h"

namespace NLS::Editor::Inspector
{
namespace
{
using namespace Engine::Serialize;
using namespace Serialization;
std::optional<std::vector<uint8_t>> Encode(const meta::Variant& value)
{
    const auto tree = NativeSerializedStreamCodec::BuildTypeTree(value.GetType(),{"Value",false});
    if (!tree) return {};
    SerializedReferenceTable refs; std::vector<uint8_t> bytes;
    if (!NativeSerializedStreamCodec::EncodeVariant(value,*tree,refs,bytes)) return {};
    return bytes;
}
bool Matches(const std::vector<std::string>& fields,const SerializedTypeTree& tree,const SerializedTypeTreeNode& node)
{
    const auto path = tree.GetPath(node.id);
    return std::any_of(fields.begin(),fields.end(),[&](const auto& field)
    { return field == node.serializedName || field == path || "Value."+field == path; });
}
ReflectedPropertyOptions CacheOptions(ReflectedPropertyOptions options)
{
    options.defaultValue={};options.inheritedValue={};options.restoreInherited={};return options;
}
bool SameOptions(const ReflectedPropertyOptions& a,const ReflectedPropertyOptions& b)
{
    if (a.hidden != b.hidden || a.disabled != b.disabled || a.choices.size() != b.choices.size() || a.metadata != b.metadata || a.layout != b.layout)
        return false;
    for (size_t i = 0; i < a.choices.size(); ++i)
        if (a.choices[i].value != b.choices[i].value || a.choices[i].label != b.choices[i].label ||
            a.choices[i].enabled != b.choices[i].enabled || a.choices[i].tooltip != b.choices[i].tooltip ||
            a.choices[i].typedValue.IsValid() != b.choices[i].typedValue.IsValid() ||
            (a.choices[i].typedValue.IsValid() && (a.choices[i].typedValue.GetType() != b.choices[i].typedValue.GetType() ||
                a.choices[i].typedValue.ToString() != b.choices[i].typedValue.ToString()))) return false;
    return true;
}
}

bool ReflectedPropertyEdit::Changed(std::string_view field) const
{
    return std::any_of(paths.begin(),paths.end(),[&](const auto& path)
    { return path == field || path.ends_with("."+std::string(field)); });
}
ReflectedValueBackend::ReflectedValueBackend(meta::Variant value,std::weak_ptr<const void> lifetime,
    ReflectedPropertyOptions options) : m_lifetime(std::move(lifetime)),m_options(CacheOptions(std::move(options)))
{
    m_values.push_back(std::move(value));
}
ReflectedValueBackend::ReflectedValueBackend(std::vector<meta::Variant> values,
    std::weak_ptr<const void> lifetime,ReflectedPropertyOptions options)
    : m_values(std::move(values)),m_lifetime(std::move(lifetime)),m_options(CacheOptions(std::move(options))) {}
bool ReflectedValueBackend::Alive() const
{
    return !m_lifetime.expired() && !m_values.empty() && m_values.front().IsValid();
}
void ReflectedValueBackend::SetSource(meta::Variant value)
{
    m_values.clear();
    m_values.push_back(std::move(value));
    ++m_revision;
}
void ReflectedValueBackend::SetSources(std::vector<meta::Variant> values)
{
    m_values = std::move(values);
    ++m_revision;
}
const meta::Variant& ReflectedValueBackend::Value() const
{
    static const meta::Variant invalid;
    return m_values.empty() ? invalid : m_values.front();
}
std::size_t ReflectedValueBackend::TargetIndex(const SerializedTargetHandle& target) const
{
    if (m_values.size() <= 1u)
        return 0u;
    char* end = nullptr;
    const auto parsed = std::strtoull(target.assetId.c_str(), &end, 10);
    if (end == target.assetId.c_str() || *end != '\0' || parsed >= m_values.size())
        return 0u;
    return static_cast<std::size_t>(parsed);
}
TypeTreeResult ReflectedValueBackend::BuildTypeTree(const SerializedTargetSet&,SerializedInspectorMode) const
{
    if (!Alive()) return {nullptr,"Reflected editing value is no longer available"};
    auto tree = NativeSerializedStreamCodec::BuildTypeTree(m_values.front().GetType(),{"Value",false});
    if (!tree) return {nullptr,"Type has no reflected serialization schema"};
    for (auto& node : tree->GetNodes())
    {
        if (Matches(m_options.hidden,*tree,node)) node.flags = node.flags|SerializedNodeFlags::HideInInspector;
        if (Matches(m_options.disabled,*tree,node)) node.flags = node.flags|SerializedNodeFlags::ReadOnly;
    }
    tree->RebuildIndexes(); return {std::move(tree),{}};
}
StreamResult ReflectedValueBackend::SerializeTarget(const SerializedTargetHandle& target,
    std::shared_ptr<const SerializedTypeTree> schema,SerializedReferenceTable& refs) const
{
    if (!Alive() || !schema) return {nullptr,"Reflected editing value is no longer available"};
    std::vector<uint8_t> bytes; std::string error;
    if (!NativeSerializedStreamCodec::EncodeVariant(
            m_values[TargetIndex(target)],*schema,refs,bytes,&error))
        return {nullptr,error};
    auto stream = std::make_shared<SerializedByteStream>(schema); stream->SetBytes(std::move(bytes));
    stream->SetReferences(std::make_shared<SerializedReferenceTable>(refs)); return {std::move(stream),{}};
}
ValidationResult ReflectedValueBackend::ValidateAndNormalize(const SerializedTargetHandle& target,
    std::span<const SerializedPropertyPath> paths,SerializedByteStream& stream) const
{
    if (!Alive() || !stream.GetSchema()) return ValidationResult::Failure("Reflected editing value was closed");
    for (const auto& path : paths)
    {
        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(std::make_shared<SerializedByteStream>(stream),path.ToString());
        const auto* node = cursor ? stream.GetSchema()->FindNode(cursor->GetNodeId()) : nullptr;
        if (!node || HasSerializedNodeFlag(node->flags,SerializedNodeFlags::ReadOnly))
            return ValidationResult::Failure("Reflected property is read only or stale: "+path.ToString());
    }
    SerializedReferenceTable beforeReferences;
    auto before = SerializeTarget(target,stream.GetSchema(),beforeReferences);
    if (!before.Succeeded()) return ValidationResult::Failure(before.error);
    for (const auto& node : stream.GetSchema()->GetNodes())
    {
        if (!HasSerializedNodeFlag(node.flags,SerializedNodeFlags::ReadOnly)) continue;
        const auto path = stream.GetSchema()->GetPath(node.id);
        auto oldValue = NativeSerializedStreamCodec::ResolveCursor(before.stream,path);
        auto newValue = NativeSerializedStreamCodec::ResolveCursor(std::make_shared<SerializedByteStream>(stream),path);
        if (!oldValue || !newValue) return ValidationResult::Failure("Read-only property is no longer available: "+path);
        std::vector<uint8_t> oldBytes(oldValue->GetByteSize()),newBytes(newValue->GetByteSize());
        if (!oldValue->Read(oldBytes.data(),oldBytes.size()) || !newValue->Read(newBytes.data(),newBytes.size()) || oldBytes != newBytes)
            return ValidationResult::Failure("Read-only property changed through its parent: "+path);
    }
    std::string error;
    const auto value = NativeSerializedStreamCodec::DecodeVariant(stream.GetBytes(),*stream.GetSchema(),
        m_values.front().GetType(),
        stream.GetReferences() ? *stream.GetReferences() : SerializedReferenceTable{},&error);
    return value ? ValidationResult::Success() : ValidationResult::Failure(error);
}
ApplyResult ReflectedValueBackend::DeserializeTarget(const SerializedTargetHandle& target,const SerializedByteStream& stream,
    const Undo::UndoExecutionContext& context) const
{
    if (!Alive() || !context.IsOperationCurrent() || !stream.GetSchema()) return ApplyResult::Failure("Reflected editing value was closed");
    const auto index = TargetIndex(target);
    std::string error;
    auto value = NativeSerializedStreamCodec::DecodeVariant(stream.GetBytes(),*stream.GetSchema(),
        m_values.front().GetType(),
        stream.GetReferences() ? *stream.GetReferences() : SerializedReferenceTable{},&error);
    if (!value) return ApplyResult::Failure(error);
    m_values[index] = std::move(*value); ++m_revision; return ApplyResult::Success();
}
SerializedVersionStamp ReflectedValueBackend::GetVersion(const SerializedTargetHandle&) const
{ return Alive() ? SerializedVersionStamp{m_revision,1,1} : SerializedVersionStamp{}; }

struct ReflectedPropertyEditor::State
{
    struct Entry
    {
        std::shared_ptr<ReflectedValueBackend> backend;
        std::unique_ptr<SerializedObject> object;
        std::vector<std::vector<uint8_t>> expected;
        std::vector<PropertyNode> projection;
        SerializedVersionStamp projectionVersion;
        uint64_t drawerVersion=0,metadataVersion=0;
        ReflectedPropertyOptions options;
    };
    InspectorPropertyEditor properties;
    std::weak_ptr<const void> lifetime;
    std::unordered_map<std::string,Entry> entries;
    bool cancelPending=false;
};
ReflectedPropertyEditor::ReflectedPropertyEditor() : m_state(std::make_unique<State>()) {}
ReflectedPropertyEditor::~ReflectedPropertyEditor() = default;
void ReflectedPropertyEditor::SetLifetime(std::weak_ptr<const void> lifetime)
{
    if (lifetime.owner_before(m_state->lifetime) || m_state->lifetime.owner_before(lifetime)) CancelEdits();
    m_state->lifetime = std::move(lifetime);
}
void ReflectedPropertyEditor::EndEdit()
{
    for (auto& [id,entry] : m_state->entries)
        if (entry.object && entry.object->HasModifiedProperties()) static_cast<void>(entry.object->ApplyModifiedPropertiesWithoutUndo());
}
void ReflectedPropertyEditor::CancelEdits()
{
    for(auto& [id,entry]:m_state->entries) if(entry.object) {m_state->cancelPending|=entry.object->HasModifiedProperties();entry.object->DiscardModifiedProperties();}
    m_state->entries.clear();m_state->properties.ClearControlState();
}
bool ReflectedPropertyEditor::TakeCanceledEdits() {return std::exchange(m_state->cancelPending,false);}
bool ReflectedPropertyEditor::HasActiveControls() const { return m_state->properties.HasActiveControls(); }
const std::string& ReflectedPropertyEditor::GetLastError() const {return m_state->properties.GetLastError();}
bool ReflectedPropertyEditor::FinishFrame()
{
    if(m_state->properties.HasInvalidatedDrawers()) {CancelEdits();return false;}
    if (HasActiveControls()) return false;
    const bool pending = std::any_of(m_state->entries.begin(),m_state->entries.end(),[](const auto& entry)
    { return entry.second.object->HasModifiedProperties(); });
    if (pending) EndEdit();
    return pending;
}
ReflectedPropertyEdit ReflectedPropertyEditor::Draw(std::string_view id,std::string_view label,meta::Variant& value,
    const ReflectedPropertyOptions& options)
{
    std::vector<meta::Variant> values;
    values.push_back(value);
    auto edit = DrawValues(id,label,std::span<meta::Variant>(values),options);
    if (edit.changed && !values.empty())
        value = std::move(values.front());
    return edit;
}
ReflectedPropertyEdit ReflectedPropertyEditor::DrawValues(std::string_view id,std::string_view label,
    std::span<meta::Variant> values,const ReflectedPropertyOptions& options)
{
    ReflectedPropertyEdit edit;
    edit.canceled=TakeCanceledEdits();
    if (m_state->lifetime.expired())
    {
        edit.canceled|=std::any_of(m_state->entries.begin(),m_state->entries.end(),[](const auto& item){return item.second.object && item.second.object->HasModifiedProperties();});
        CancelEdits();return edit;
    }
    if(values.empty()) return edit;

    std::vector<std::vector<uint8_t>> sources;
    sources.reserve(values.size());
    for (const auto& value : values)
    {
        auto source = Encode(value);
        if (!source) { ImGui::TextDisabled("Type is not reflected"); return edit; }
        sources.push_back(std::move(*source));
    }

    const auto scopedId=std::to_string(ImGui::GetID(std::string(id).c_str()));
    auto& entry = m_state->entries[scopedId];
    const bool recreate = !entry.backend || !entry.backend->Alive() ||
        entry.backend->Values().size() != values.size() ||
        entry.backend->Value().GetType() != values.front().GetType() ||
        !SameOptions(entry.options,options);
    if (recreate || entry.expected != sources)
    {
        if (entry.object) {edit.canceled=entry.object->HasModifiedProperties();entry.object->DiscardModifiedProperties();}
        entry = {};
        std::vector<meta::Variant> copies(values.begin(),values.end());
        entry.backend = std::make_shared<ReflectedValueBackend>(std::move(copies),m_state->lifetime,options);
        SerializedTargetSet targets;
        targets.targets.reserve(values.size());
        for (size_t index = 0u; index < values.size(); ++index)
        {
            auto target = InspectorTargetHandle::FromAsset(
                InspectorTargetKind::AssetImporter,
                scopedId + "[" + std::to_string(index) + "]",
                std::to_string(index),
                "ReflectedProperties");
            targets.targets.push_back(std::move(target));
        }
        entry.object = std::make_unique<SerializedObject>(std::move(targets),entry.backend);
        entry.expected = sources; entry.options = CacheOptions(options);
    }
    if(edit.canceled) {ImGui::TextDisabled("Property changed externally; edit canceled");return edit;}
    if (!entry.object->IsValid()) { ImGui::TextDisabled("%s",entry.object->GetLastError().c_str()); return edit; }
    const auto version=entry.object->GetVersion();
    const auto drawerVersion=InspectorRegistry::Get().GetVersion();
    const auto metadataVersion=InspectorMetadataRegistry::Get().GetVersion();
    if(entry.projection.empty() || entry.projectionVersion.schemaGeneration!=version.schemaGeneration ||
        entry.projectionVersion.structureGeneration!=version.structureGeneration || entry.drawerVersion!=drawerVersion || entry.metadataVersion!=metadataVersion)
    {
        entry.projection=InspectorProjectionBuilder::BuildPropertyProjection(*entry.object,entry.object->GetTargetSet(),{});
        auto& properties=entry.projection;
    if (properties.size() == 1 && properties.front().path.ToString() == "Value")
    {
        const auto rootDrawer = Properties::PropertyDrawerRegistry::Get().Resolve(properties.front());
        if (properties.front().kind == InspectorPropertyKind::Struct &&
            (!rootDrawer || rootDrawer->scope == InspectorDrawerScope::Builtin))
        {
            auto children = std::move(properties.front().children);
            properties = std::move(children);
        }
        else
        {
            properties.front().displayName = label;
            if (!options.choices.empty())
            { properties.front().kind = InspectorPropertyKind::Choice; properties.front().choices = options.choices; }
        }
    }
    if (options.metadata && properties.size() == 1u)
    {
        properties.front().metadata = *options.metadata;
        if(properties.front().metadata.flags && properties.front().kind==InspectorPropertyKind::Enum)
            properties.front().kind=InspectorPropertyKind::Flags;
    }
        entry.projectionVersion=version;entry.drawerVersion=drawerVersion;entry.metadataVersion=metadataVersion;
    }
    const auto& properties=entry.projection;
    ImGui::PushID(std::string(id).c_str());
    PropertyEditorContext context;
    context.object = entry.object.get();
    if(options.defaultValue) context.defaultValue=[&options](const PropertyNode& node){return options.defaultValue(node.path.ToString());};
    if(options.inheritedValue) context.inheritedValue=[&options](const PropertyNode& node){return options.inheritedValue(node.path.ToString());};
    if(options.restoreInherited) context.restoreInherited=[&options](const PropertyNode&){return options.restoreInherited();};

    context.apply = [&entry]
    { return entry.object->ApplyModifiedPropertiesWithoutUndo(); };
    auto result = options.layout == PropertyEditorLayout::Properties ? m_state->properties.DrawProperties(properties, context) : properties.size() == 1u ? m_state->properties.DrawProperty(properties.front(), context, options.layout) :
                                                                                                                                                           m_state->properties.DrawProperties(properties, context);
    ImGui::PopID();
    edit.completed = result.completed; edit.canceled |= result.canceled; edit.paths = std::move(result.changedPaths);
    if(result.hostAction) {entry.object->DiscardModifiedProperties();entry={};return edit;}
    auto staged = entry.object->FindProperty("Value").GetValue();
    const auto bytes = staged.IsValid() ? Encode(staged) : std::nullopt;
    if (bytes)
    {
        edit.changed = !edit.canceled && (*bytes != sources.front() || !edit.paths.empty());
        if (edit.changed)
        {
            const auto stagedValues=entry.object->FindProperty("Value").GetTargetValues();
            if(stagedValues.size()==values.size()) std::copy(stagedValues.begin(),stagedValues.end(),values.begin());
            else values.front()=staged;
            entry.expected.clear();
            for(const auto& targetValue:values) {const auto encoded=Encode(targetValue);if(encoded) entry.expected.push_back(*encoded);}
        }
    }
    return edit;
}
}
