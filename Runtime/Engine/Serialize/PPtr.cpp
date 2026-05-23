#include "Serialize/PPtr.h"

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
    NLS::Object::AssignInstanceID(&object, instanceID);
    return instanceID;
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
