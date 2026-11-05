#include "Assets/PrefabSceneExpansion.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace NLS::Engine::Assets
{
namespace
{
using namespace Serialize;
using Index = std::unordered_map<ObjectId, size_t>;
using IdMap = std::unordered_map<ObjectId, ObjectId>;

Index MakeIndex(const ObjectGraphDocument& document)
{
    Index index;
    index.reserve(document.objects.size());
    for (size_t i = 0; i < document.objects.size(); ++i) index.emplace(document.objects[i].id, i);
    return index;
}

PropertyValue* Property(ObjectRecord& record, const std::string& name)
{
    for (auto& property : record.properties) if (property.name == name) return &property.value;
    return nullptr;
}

const PropertyValue* Property(const ObjectRecord& record, const std::string& name)
{
    for (const auto& property : record.properties) if (property.name == name) return &property.value;
    return nullptr;
}

std::vector<ObjectId> Components(const ObjectRecord& record)
{
    std::vector<ObjectId> ids;
    const auto* value = Property(record, "components");
    if (value && value->GetKind() == PropertyValue::Kind::Array)
        for (const auto& item : value->GetArray())
            if (item.GetKind() == PropertyValue::Kind::OwnedReference) ids.push_back(item.GetObjectId());
    return ids;
}

void Remap(PropertyValue& value, const IdMap& ids, const std::unordered_map<int64_t, int64_t>& files)
{
    switch (value.GetKind())
    {
    case PropertyValue::Kind::OwnedReference:
        if (const auto found = ids.find(value.GetObjectId()); found != ids.end())
            value = PropertyValue::OwnedReference(found->second);
        break;
    case PropertyValue::Kind::ObjectReference:
        if (auto& reference = value.GetMutableObjectReference(); reference.IsLocalObject())
            if (const auto found = files.find(reference.localIdentifierInFile); found != files.end())
                reference.localIdentifierInFile = found->second;
        break;
    case PropertyValue::Kind::Array:
        for (auto& item : value.GetMutableArray()) Remap(item, ids, files);
        break;
    case PropertyValue::Kind::Object:
        for (auto& item : value.GetMutableObject()) Remap(item.second, ids, files);
        break;
    default: break;
    }
}

class Expander
{
public:
    const PrefabGraphResolver& resolver;
    std::string& error;
    std::unordered_map<std::string, ObjectGraphDocument> cache;
    std::unordered_set<std::string> active;

    bool Visit(ObjectGraphDocument& document, size_t depth)
    {
        if (depth > 64) { error = "Prefab nesting exceeds 64 levels."; return false; }
        const auto instances = document.prefabInstances;
        for (const auto& instance : instances)
        {
            const auto key = instance.sourcePrefab.guid.ToString() + ":" +
                std::to_string(instance.sourcePrefab.localIdentifierInFile) +
                (instance.sourcePrefab.localIdentifierInFile ? std::string {} : ":" + instance.sourcePrefab.filePath);
            if (!instance.sourcePrefab.IsAsset()) { error = "Invalid Prefab source reference."; return false; }
            if (!cache.contains(key))
            {
                if (!active.insert(key).second) { error = "Prefab dependency cycle: " + key; return false; }
                auto source = resolver ? resolver(instance.sourcePrefab, error) : std::nullopt;
                if (!source) { if (error.empty()) error = "Missing cooked Prefab: " + key; return false; }
                if (source->format != "Nullus.ObjectGraph.Prefab") { error = "Cooked Prefab has an invalid format."; return false; }
                for (const auto& patch : source->overrides)
                    if (patch.type == PatchOperationType::AddPrefabInstance)
                    { error = "Prefab insertion patches must be materialized before runtime scene expansion."; return false; }
                *source = ObjectGraphInstantiator::ResolvePrefabOverrides(std::move(*source));
                if (!Visit(*source, depth + 1)) return false;
                active.erase(key);
                cache.emplace(key, std::move(*source));
            }
            auto source = cache.at(key);
            auto sourceIndex = MakeIndex(source);
            for (const auto& added : instance.addedObjects)
            {
                if (sourceIndex.contains(added.id)) { error = "Prefab added object duplicates a source id."; return false; }
                sourceIndex.emplace(added.id, source.objects.size());
                source.objects.push_back(added);
            }
            source.overrides = instance.modifications;
            for (const auto& patch : source.overrides)
            {
                if (patch.type == PatchOperationType::AddPrefabInstance)
                { error = "Prefab insertion patches must be materialized before runtime scene expansion."; return false; }
                if (!sourceIndex.contains(patch.target)) { error = "Prefab override targets a missing source object."; return false; }
            }
            source = ObjectGraphInstantiator::ResolvePrefabOverrides(std::move(source));
            if (!Merge(document, source, instance)) return false;
        }
        document.prefabInstances.clear();
        for (const auto& object : document.objects)
            if (object.state == ObjectRecordState::Stripped) { error = "Unresolved stripped Prefab object."; return false; }
        const auto diagnostics = document.Validate();
        if (diagnostics.HasErrors())
        {
            error = "Expanded Prefab graph is invalid.";
            for (const auto& item : diagnostics.GetItems()) error += " " + item.GetMessage();
            return false;
        }
        return true;
    }

private:
    bool Merge(ObjectGraphDocument& scene, const ObjectGraphDocument& source, const Serialize::PrefabInstanceRecord& instance)
    {
        auto sceneIndex = MakeIndex(scene);
        const auto sourceIndex = MakeIndex(source);
        if (!sourceIndex.contains(source.root) || !sceneIndex.contains(instance.instanceRoot))
        { error = "Prefab instance root is missing."; return false; }
        IdMap ids;
        for (const auto& pair : instance.correspondence) ids[pair.sourceObject] = pair.instanceObject;
        ids[source.root] = instance.instanceRoot;
        for (const auto& added : instance.addedObjects) ids.try_emplace(added.id, added.id);

        // Scene correspondence tracks GameObjects. Recover existing component
        // identities by owner and type/order, including duplicate component types.
        for (const auto& owner : source.objects)
        {
            const auto mappedOwner = ids.find(owner.id);
            if (mappedOwner == ids.end() || !sceneIndex.contains(mappedOwner->second)) continue;
            const auto previous = Components(scene.objects[sceneIndex.at(mappedOwner->second)]);
            std::unordered_set<ObjectId> matched;
            for (const auto& componentId : Components(owner))
                if (const auto mapped = ids.find(componentId); mapped != ids.end()) matched.insert(mapped->second);
            for (const auto& componentId : Components(owner))
            {
                if (ids.contains(componentId) || !sourceIndex.contains(componentId)) continue;
                const auto& component = source.objects[sourceIndex.at(componentId)];
                for (const auto& oldId : previous)
                {
                    if (matched.contains(oldId) || !sceneIndex.contains(oldId)) continue;
                    if (scene.objects[sceneIndex.at(oldId)].typeName != component.typeName) continue;
                    ids.emplace(componentId, oldId);
                    matched.insert(oldId);
                    break;
                }
            }
        }
        std::unordered_set<ObjectId> destinations;
        std::unordered_set<int64_t> usedFiles;
        for (const auto& record : scene.objects) usedFiles.insert(record.localIdentifierInFile);
        std::unordered_map<int64_t, int64_t> files;
        std::unordered_map<ObjectId, int64_t> destinationFiles;
        for (const auto& record : source.objects)
        {
            ids.try_emplace(record.id, ObjectId(NLS::Guid::NewDeterministic(
                "Prefab.Scene:" + instance.instanceRoot.GetGuid().ToString() + ":" + record.id.GetGuid().ToString())));
            const auto destination = ids.at(record.id);
            if (!destinations.insert(destination).second) { error = "Prefab correspondence contains duplicate destinations."; return false; }
            int64_t file = 0;
            if (const auto old = sceneIndex.find(destination); old != sceneIndex.end())
            {
                const auto& previous = scene.objects[old->second];
                if (previous.typeName != record.typeName) { error = "Prefab correspondence has incompatible object types."; return false; }
                file = previous.localIdentifierInFile;
            }
            if (!file)
            {
                file = MakeLocalIdentifierInFile(destination);
                while (!file || usedFiles.contains(file)) file = file == INT64_MAX ? 1 : file + 1;
                usedFiles.insert(file);
            }
            destinationFiles.emplace(record.id, file);
            if (record.localIdentifierInFile) files.emplace(record.localIdentifierInFile, file);
        }

        // Unity-style root placement is scene-local; the Prefab still supplies
        // component values, scale and all explicitly recorded overrides.
        std::unordered_map<std::string, PropertyValue> rootPlacement;
        ObjectId sourceTransform;
        for (const auto& componentId : Components(source.objects[sourceIndex.at(source.root)]))
        {
            if (!sourceIndex.contains(componentId)) continue;
            if (!source.objects[sourceIndex.at(componentId)].typeName.ends_with("::TransformComponent")) continue;
            sourceTransform = componentId;
            const auto old = sceneIndex.find(ids.at(componentId));
            if (old != sceneIndex.end())
                for (const auto* name : {"localPosition", "localRotation"})
                    if (const auto* value = Property(scene.objects[old->second], name)) rootPlacement.emplace(name, *value);
            break;
        }
        std::unordered_set<ObjectId> previousStripped;
        std::function<void(const ObjectId&)> collect = [&](const ObjectId& id)
        {
            const auto found = sceneIndex.find(id);
            if (found == sceneIndex.end() || scene.objects[found->second].state != ObjectRecordState::Stripped ||
                !previousStripped.insert(id).second) return;
            std::function<void(const PropertyValue&)> owned = [&](const PropertyValue& value)
            {
                if (value.GetKind() == PropertyValue::Kind::OwnedReference) collect(value.GetObjectId());
                else if (value.GetKind() == PropertyValue::Kind::Array) for (const auto& item : value.GetArray()) owned(item);
                else if (value.GetKind() == PropertyValue::Kind::Object) for (const auto& item : value.GetObject()) owned(item.second);
            };
            for (const auto& property : scene.objects[found->second].properties) owned(property.value);
        };
        collect(instance.instanceRoot);
        if (scene.objects.size() + source.objects.size() > 1000000)
        { error = "Expanded Prefab scene exceeds the object limit."; return false; }
        for (auto record : source.objects)
        {
            const auto sourceId = record.id;
            record.id = ids.at(sourceId);
            record.localIdentifierInFile = destinationFiles.at(sourceId);
            for (auto& property : record.properties) Remap(property.value, ids, files);
            if (sourceId == sourceTransform)
                for (const auto& [name, value] : rootPlacement)
                    if (auto* property = Property(record, name)) *property = value;
            if (const auto old = sceneIndex.find(record.id); old != sceneIndex.end()) scene.objects[old->second] = std::move(record);
            else { sceneIndex.emplace(record.id, scene.objects.size()); scene.objects.push_back(std::move(record)); }
        }
        std::erase_if(scene.objects, [&](const auto& record) { return previousStripped.contains(record.id) && !destinations.contains(record.id); });
        return true;
    }
};
}

bool ExpandPrefabInstances(ObjectGraphDocument& document, const PrefabGraphResolver& resolver, std::string& error)
{
    error.clear();
    if (document.objects.size() > 1000000) { error = "Prefab scene exceeds the object limit."; return false; }
    if (document.prefabInstances.empty())
    {
        if (std::none_of(document.objects.begin(), document.objects.end(), [](const auto& object) { return object.state == ObjectRecordState::Stripped; }))
            return true;
    }
    auto expanded = document;
    if (expanded.Validate().HasErrors()) { error = "Cannot expand an invalid authoring scene graph."; return false; }
    Expander expander {resolver, error};
    if (!expander.Visit(expanded, 0)) return false;
    document = std::move(expanded);
    return true;
}
}
