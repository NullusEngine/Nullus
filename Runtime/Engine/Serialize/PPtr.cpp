#include "Serialize/PPtr.h"

#include <limits>

namespace NLS::Engine::Serialize
{
PersistentManager& PersistentManager::Instance()
{
    static PersistentManager instance;
    return instance;
}

void PersistentManager::Clear()
{
    std::lock_guard lock(m_mutex);
    for (const auto& [instanceID, identifier] : m_identifierByInstanceID)
    {
        if (NLS::Object::IDToPointer(instanceID) == nullptr)
            NLS::Object::ReleaseReservedInstanceID(instanceID);
    }
    m_identifierByInstanceID.clear();
    m_instanceIDByIdentifier.clear();
    m_serializedFileIndexByFileIdentifier.clear();
    m_fileIdentifierBySerializedFileIndex.clear();
    m_nextSerializedFileIndex = 1;
    m_objectGraphScopes.clear();
}

std::optional<ObjectId> PersistentManager::FindObjectGraphId(
    const NLS::Object& scope, const NLS::Object& object) const
{
    std::lock_guard lock(m_mutex);
    const auto found = m_objectGraphScopes.find(scope.GetInstanceID());
    if (found == m_objectGraphScopes.end()) return std::nullopt;
    const auto entry = found->second.byInstance.find(object.GetInstanceID());
    return entry == found->second.byInstance.end() ? std::nullopt : std::optional(entry->second);
}

ObjectId PersistentManager::GetOrCreateObjectGraphId(
    const NLS::Object& scope, const NLS::Object& object)
{
    std::lock_guard lock(m_mutex);
    auto& ids = m_objectGraphScopes[scope.GetInstanceID()];
    if (const auto found = ids.byInstance.find(object.GetInstanceID()); found != ids.byInstance.end())
        return found->second;
    const ObjectId id(NLS::Guid::New());
    ids.byInstance.emplace(object.GetInstanceID(), id);
    ids.byId.emplace(id, object.GetInstanceID());
    return id;
}

bool PersistentManager::BindObjectGraphId(
    const NLS::Object& scope, const NLS::Object& object, ObjectId id)
{
    if (!id.IsValid()) return false;
    std::lock_guard lock(m_mutex);
    auto& ids = m_objectGraphScopes[scope.GetInstanceID()];
    if (const auto owner = ids.byId.find(id); owner != ids.byId.end() && owner->second != object.GetInstanceID())
    {
        if (NLS::Object::IDToPointer(owner->second) != nullptr) return false;
        ids.byInstance.erase(owner->second);
        ids.byId.erase(owner);
    }
    if (const auto old = ids.byInstance.find(object.GetInstanceID()); old != ids.byInstance.end())
        ids.byId.erase(old->second);
    ids.byInstance[object.GetInstanceID()] = id;
    ids.byId[id] = object.GetInstanceID();
    return true;
}

void PersistentManager::PruneObjectGraphIds(const NLS::Object& scope)
{
    std::lock_guard lock(m_mutex);
    const auto found = m_objectGraphScopes.find(scope.GetInstanceID());
    if (found == m_objectGraphScopes.end()) return;
    auto& ids = found->second;
    for (auto entry = ids.byInstance.begin(); entry != ids.byInstance.end();)
    {
        if (NLS::Object::IDToPointer(entry->first) != nullptr) { ++entry; continue; }
        ids.byId.erase(entry->second);
        entry = ids.byInstance.erase(entry);
    }
}

void PersistentManager::ClearObjectGraphIds(const NLS::Object& scope)
{
    std::lock_guard lock(m_mutex);
    m_objectGraphScopes.erase(scope.GetInstanceID());
}

std::optional<NLS::Guid> PersistentManager::FindObjectGraphDocumentId(const NLS::Object& scope) const
{
    std::lock_guard lock(m_mutex);
    const auto found = m_objectGraphScopes.find(scope.GetInstanceID());
    return found == m_objectGraphScopes.end() ? std::nullopt : found->second.documentId;
}

void PersistentManager::BindObjectGraphDocumentId(const NLS::Object& scope, const NLS::Guid& id)
{
    std::lock_guard lock(m_mutex);
    m_objectGraphScopes[scope.GetInstanceID()].documentId = id;
}

bool PersistentManager::SamePersistentIdentity(
    const ObjectIdentifier& lhs,
    const ObjectIdentifier& rhs)
{
    return lhs.localIdentifierInFile == rhs.localIdentifierInFile &&
           lhs.guid == rhs.guid &&
           lhs.fileType == rhs.fileType;
}

FileIdentifier PersistentManager::MakeFileIdentifier(const ObjectIdentifier& identifier)
{
    return {
        identifier.guid,
        identifier.fileType,
        identifier.filePath
    };
}

void PersistentManager::RefreshPathHintLocked(InstanceID instanceID, const ObjectIdentifier& hint)
{
    if (instanceID == InstanceID_None || hint.filePath.empty())
        return;

    const auto existing = m_identifierByInstanceID.find(instanceID);
    if (existing == m_identifierByInstanceID.end() ||
        !SamePersistentIdentity(existing->second, hint) ||
        existing->second.filePath == hint.filePath)
    {
        return;
    }

    ObjectIdentifier updated = existing->second;
    updated.filePath = hint.filePath;
    m_instanceIDByIdentifier.erase(existing->second);
    existing->second = updated;
    m_instanceIDByIdentifier[updated] = instanceID;
    RegisterExternalFileLocked(updated);
}

void PersistentManager::RegisterExternalFileLocked(const ObjectIdentifier& identifier)
{
    if (!identifier.IsAsset())
        return;

    const auto file = MakeFileIdentifier(identifier);
    if (const auto found = m_serializedFileIndexByFileIdentifier.find(file);
        found != m_serializedFileIndexByFileIdentifier.end())
    {
        if (!identifier.filePath.empty())
            m_fileIdentifierBySerializedFileIndex[found->second].path = identifier.filePath;
        return;
    }

    const auto fileIndex = m_nextSerializedFileIndex++;
    m_serializedFileIndexByFileIdentifier.emplace(file, fileIndex);
    m_fileIdentifierBySerializedFileIndex.emplace(fileIndex, file);
}

void PersistentManager::ReleaseReservedInstanceIDIfOrphanedLocked(InstanceID instanceID) const
{
    if (instanceID == InstanceID_None || NLS::Object::IDToPointer(instanceID) != nullptr)
        return;

    NLS::Object::ReleaseReservedInstanceID(instanceID);
}

void PersistentManager::EraseInstanceMappingLocked(InstanceID instanceID, bool releaseReservedInstanceID)
{
    const auto existingIdentifier = m_identifierByInstanceID.find(instanceID);
    if (existingIdentifier == m_identifierByInstanceID.end())
        return;

    m_instanceIDByIdentifier.erase(existingIdentifier->second);
    m_identifierByInstanceID.erase(existingIdentifier);
    if (releaseReservedInstanceID)
        ReleaseReservedInstanceIDIfOrphanedLocked(instanceID);
}

int32_t PersistentManager::GetOrCreateExternalFileIndexLocked(const ObjectIdentifier& identifier) const
{
    if (!identifier.IsAsset())
        return 0;

    const auto file = MakeFileIdentifier(identifier);
    if (const auto found = m_serializedFileIndexByFileIdentifier.find(file);
        found != m_serializedFileIndexByFileIdentifier.end())
    {
        if (!identifier.filePath.empty())
            m_fileIdentifierBySerializedFileIndex[found->second].path = identifier.filePath;
        return found->second;
    }

    const auto fileIndex = m_nextSerializedFileIndex++;
    m_serializedFileIndexByFileIdentifier.emplace(file, fileIndex);
    m_fileIdentifierBySerializedFileIndex.emplace(fileIndex, file);
    return fileIndex;
}

InstanceID PersistentManager::RegisterObjectIdentifier(const ObjectIdentifier& identifier)
{
    if (!identifier.IsValid())
        return InstanceID_None;

    std::lock_guard lock(m_mutex);
    if (const auto found = m_instanceIDByIdentifier.find(identifier); found != m_instanceIDByIdentifier.end())
    {
        const auto instanceID = found->second;
        RefreshPathHintLocked(instanceID, identifier);
        return instanceID;
    }

    const InstanceID instanceID = NLS::Object::ReserveInstanceID();
    m_identifierByInstanceID.emplace(instanceID, identifier);
    m_instanceIDByIdentifier.emplace(identifier, instanceID);
    RegisterExternalFileLocked(identifier);
    return instanceID;
}

void PersistentManager::RegisterInstanceID(InstanceID instanceID, const ObjectIdentifier& identifier)
{
    if (instanceID == InstanceID_None || !identifier.IsValid())
        return;

    std::lock_guard lock(m_mutex);
    if (NLS::Object::IDToPointer(instanceID) == nullptr)
        NLS::Object::ReserveInstanceID(instanceID);

    if (const auto existingIdentifier = m_identifierByInstanceID.find(instanceID);
        existingIdentifier != m_identifierByInstanceID.end())
    {
        m_instanceIDByIdentifier.erase(existingIdentifier->second);
    }

    if (const auto existingInstanceID = m_instanceIDByIdentifier.find(identifier);
        existingInstanceID != m_instanceIDByIdentifier.end())
    {
        const auto conflictingInstanceID = existingInstanceID->second;
        RefreshPathHintLocked(conflictingInstanceID, identifier);
        EraseInstanceMappingLocked(conflictingInstanceID, true);
    }

    m_identifierByInstanceID[instanceID] = identifier;
    m_instanceIDByIdentifier[identifier] = instanceID;
    RegisterExternalFileLocked(identifier);
}

InstanceID PersistentManager::BindObjectIdentifier(NLS::Object& object, const ObjectIdentifier& identifier)
{
    if (!identifier.IsValid())
        return InstanceID_None;

    std::lock_guard lock(m_mutex);
    const InstanceID oldObjectInstanceID = object.GetInstanceID();
    InstanceID instanceID = InstanceID_None;
    if (const auto found = m_instanceIDByIdentifier.find(identifier); found != m_instanceIDByIdentifier.end())
    {
        instanceID = found->second;
        RefreshPathHintLocked(instanceID, identifier);
    }
    else
        instanceID = oldObjectInstanceID != InstanceID_None
            ? oldObjectInstanceID
            : NLS::Object::ReserveInstanceID();

    if (auto* existingObject = NLS::Object::IDToPointer(instanceID);
        existingObject != nullptr && existingObject != &object)
    {
        return InstanceID_None;
    }

    if (oldObjectInstanceID != InstanceID_None && oldObjectInstanceID != instanceID)
    {
        if (const auto oldIdentifier = m_identifierByInstanceID.find(oldObjectInstanceID);
            oldIdentifier != m_identifierByInstanceID.end())
        {
            m_instanceIDByIdentifier.erase(oldIdentifier->second);
            m_identifierByInstanceID.erase(oldIdentifier);
        }
    }

    if (const auto existingIdentifier = m_identifierByInstanceID.find(instanceID);
        existingIdentifier != m_identifierByInstanceID.end())
    {
        m_instanceIDByIdentifier.erase(existingIdentifier->second);
    }

    if (const auto existingInstanceID = m_instanceIDByIdentifier.find(identifier);
        existingInstanceID != m_instanceIDByIdentifier.end())
    {
        const auto conflictingInstanceID = existingInstanceID->second;
        RefreshPathHintLocked(conflictingInstanceID, identifier);
        EraseInstanceMappingLocked(conflictingInstanceID, true);
    }

    m_identifierByInstanceID[instanceID] = identifier;
    m_instanceIDByIdentifier[identifier] = instanceID;
    RegisterExternalFileLocked(identifier);
    if (oldObjectInstanceID != instanceID)
    {
        for (auto& [scope, ids] : m_objectGraphScopes)
        {
            if (const auto old = ids.byInstance.find(oldObjectInstanceID); old != ids.byInstance.end())
            {
                const auto id = old->second;
                ids.byInstance.erase(old);
                if (const auto occupied = ids.byInstance.find(instanceID); occupied != ids.byInstance.end())
                {
                    ids.byId.erase(occupied->second);
                    ids.byInstance.erase(occupied);
                }
                ids.byInstance[instanceID] = id;
                ids.byId[id] = instanceID;
            }
        }
        if (auto scope = m_objectGraphScopes.extract(oldObjectInstanceID); !scope.empty())
        {
            scope.key() = instanceID;
            m_objectGraphScopes.insert(std::move(scope));
        }
    }
    NLS::Object::AssignInstanceID(&object, instanceID);
    return instanceID;
}

ObjectIdentifier PersistentManager::GetOrCreateLocalObjectIdentifier(NLS::Object& object)
{
    const InstanceID instanceID = object.GetInstanceID();
    if (instanceID == InstanceID_None)
        return {};

    std::lock_guard lock(m_mutex);
    if (NLS::Object::IDToPointer(instanceID) != &object)
        return {};

    if (const auto existing = m_identifierByInstanceID.find(instanceID);
        existing != m_identifierByInstanceID.end())
    {
        // A scene object must not overwrite an asset identity that another
        // system already attached to this Object instance.
        return existing->second.IsLocalObject() ? existing->second : ObjectIdentifier {};
    }

    while (m_nextRuntimeLocalIdentifier < 0)
    {
        const auto localIdentifier = m_nextRuntimeLocalIdentifier;
        if (m_nextRuntimeLocalIdentifier == std::numeric_limits<int64_t>::min())
            m_nextRuntimeLocalIdentifier = 0;
        else
            --m_nextRuntimeLocalIdentifier;

        const auto identifier = ObjectIdentifier::LocalObject(localIdentifier);
        if (m_instanceIDByIdentifier.contains(identifier))
            continue;

        m_identifierByInstanceID.emplace(instanceID, identifier);
        m_instanceIDByIdentifier.emplace(identifier, instanceID);
        return identifier;
    }

    // Exhaustion would require more than 2^63 runtime object identities in a
    // single process.  Returning an invalid identifier fails the command
    // safely instead of reusing a stale target identity.
    return {};
}

bool PersistentManager::InstanceIDToObjectIdentifier(InstanceID instanceID, ObjectIdentifier& identifier) const
{
    std::lock_guard lock(m_mutex);
    const auto found = m_identifierByInstanceID.find(instanceID);
    if (found == m_identifierByInstanceID.end())
        return false;

    identifier = found->second;
    return true;
}

bool PersistentManager::TryGetInstanceID(
    const ObjectIdentifier& identifier,
    InstanceID& instanceID) const
{
    instanceID = InstanceID_None;
    if (!identifier.IsValid())
        return false;

    std::lock_guard lock(m_mutex);
    const auto found = m_instanceIDByIdentifier.find(identifier);
    if (found == m_instanceIDByIdentifier.end())
        return false;

    instanceID = found->second;
    return true;
}

bool PersistentManager::ReserveObjectIdentifier(const ObjectIdentifier& identifier)
{
    if (!identifier.IsValid())
        return false;

    std::lock_guard lock(m_mutex);
    const auto found = m_instanceIDByIdentifier.find(identifier);
    if (found == m_instanceIDByIdentifier.end() || found->second == InstanceID_None ||
        NLS::Object::IDToPointer(found->second) != nullptr)
    {
        return false;
    }

    NLS::Object::ReserveInstanceID(found->second);
    return true;
}

void PersistentManager::ReleaseReservedObjectIdentifier(const ObjectIdentifier& identifier)
{
    if (!identifier.IsValid())
        return;

    std::lock_guard lock(m_mutex);
    const auto found = m_instanceIDByIdentifier.find(identifier);
    if (found == m_instanceIDByIdentifier.end() || found->second == InstanceID_None ||
        NLS::Object::IDToPointer(found->second) != nullptr)
    {
        return;
    }

    NLS::Object::ReleaseReservedInstanceID(found->second);
}

bool PersistentManager::InstanceIDToSerializedObjectIdentifier(
    InstanceID instanceID,
    SerializedObjectIdentifier& identifier) const
{
    std::lock_guard lock(m_mutex);
    const auto found = m_identifierByInstanceID.find(instanceID);
    if (found == m_identifierByInstanceID.end())
        return false;

    identifier = {
        found->second.IsAsset() ? GetOrCreateExternalFileIndexLocked(found->second) : 0,
        found->second.localIdentifierInFile
    };
    return true;
}

InstanceID PersistentManager::ObjectIdentifierToInstanceID(const ObjectIdentifier& identifier)
{
    if (!identifier.IsValid())
        return InstanceID_None;

    std::lock_guard lock(m_mutex);
    if (const auto found = m_instanceIDByIdentifier.find(identifier); found != m_instanceIDByIdentifier.end())
    {
        const auto instanceID = found->second;
        RefreshPathHintLocked(instanceID, identifier);
        return instanceID;
    }

    const InstanceID instanceID = NLS::Object::ReserveInstanceID();
    m_identifierByInstanceID.emplace(instanceID, identifier);
    m_instanceIDByIdentifier.emplace(identifier, instanceID);
    RegisterExternalFileLocked(identifier);
    return instanceID;
}

void PersistentManager::InstanceIDToLocalSerializedObjectIdentifier(
    InstanceID instanceID,
    LocalSerializedObjectIdentifier& identifier) const
{
    SerializedObjectIdentifier serializedIdentifier;
    if (!InstanceIDToSerializedObjectIdentifier(instanceID, serializedIdentifier))
    {
        identifier = {};
        return;
    }

    identifier.localSerializedFileIndex = serializedIdentifier.serializedFileIndex;
    identifier.localIdentifierInFile = serializedIdentifier.localIdentifierInFile;
}

void PersistentManager::LocalSerializedObjectIdentifierToInstanceID(
    const LocalSerializedObjectIdentifier& identifier,
    InstanceID& instanceID)
{
    if (identifier.localIdentifierInFile == 0)
    {
        instanceID = InstanceID_None;
        return;
    }

    if (identifier.localSerializedFileIndex == 0)
    {
        instanceID = ObjectIdentifierToInstanceID(ObjectIdentifier::LocalObject(identifier.localIdentifierInFile));
        return;
    }

    std::lock_guard lock(m_mutex);
    const auto file = m_fileIdentifierBySerializedFileIndex.find(identifier.localSerializedFileIndex);
    if (file == m_fileIdentifierBySerializedFileIndex.end())
    {
        instanceID = InstanceID_None;
        return;
    }

    ObjectIdentifier objectIdentifier;
    objectIdentifier.guid = file->second.guid;
    objectIdentifier.fileType = file->second.type;
    objectIdentifier.filePath = file->second.path;
    objectIdentifier.localIdentifierInFile = identifier.localIdentifierInFile;
    if (!objectIdentifier.IsValid())
    {
        instanceID = InstanceID_None;
        return;
    }

    if (const auto found = m_instanceIDByIdentifier.find(objectIdentifier); found != m_instanceIDByIdentifier.end())
    {
        instanceID = found->second;
        RefreshPathHintLocked(instanceID, objectIdentifier);
        return;
    }

    instanceID = NLS::Object::ReserveInstanceID();
    m_identifierByInstanceID.emplace(instanceID, objectIdentifier);
    m_instanceIDByIdentifier.emplace(objectIdentifier, instanceID);
}

}
