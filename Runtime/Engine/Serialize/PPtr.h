#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "EngineDef.h"
#include "Guid.h"
#include "Object/Object.h"
#include "Reflection/TypeConfig.h"
#include "Serialize/ObjectId.h"

namespace NLS::Engine::Serialize
{
    using InstanceID = NLS::InstanceID;
    inline constexpr InstanceID InstanceID_None = NLS::InstanceID_None;

    enum class FileType : int32_t
    {
        NonAssetType = 0,
        DeprecatedCachedAssetType = 1,
        SerializedAssetType = 2,
        MetaAssetType = 3
    };

    inline bool IsKnownFileType(FileType type)
    {
        switch (type)
        {
        case FileType::NonAssetType:
        case FileType::DeprecatedCachedAssetType:
        case FileType::SerializedAssetType:
        case FileType::MetaAssetType:
            return true;
        default:
            return false;
        }
    }

    struct LocalSerializedObjectIdentifier
    {
        int32_t localSerializedFileIndex = 0;
        int64_t localIdentifierInFile = 0;

        friend bool operator==(const LocalSerializedObjectIdentifier& lhs, const LocalSerializedObjectIdentifier& rhs) = default;
    };

    struct FileIdentifier
    {
        NLS::Guid guid;
        FileType type = FileType::NonAssetType;
        std::string path;

        friend bool operator==(const FileIdentifier& lhs, const FileIdentifier& rhs)
        {
            return lhs.guid == rhs.guid &&
                   lhs.type == rhs.type;
        }
    };

    struct SerializedObjectIdentifier
    {
        int32_t serializedFileIndex = 0;
        int64_t localIdentifierInFile = 0;

        friend bool operator==(const SerializedObjectIdentifier& lhs, const SerializedObjectIdentifier& rhs) = default;
    };

    struct ObjectIdentifier
    {
        NLS::Guid guid;
        int64_t localIdentifierInFile = 0;
        FileType fileType = FileType::NonAssetType;
        std::string filePath;

        static ObjectIdentifier LocalObject(int64_t localIdentifierInFile)
        {
            ObjectIdentifier identifier;
            identifier.localIdentifierInFile = localIdentifierInFile;
            identifier.fileType = FileType::NonAssetType;
            return identifier;
        }

        static ObjectIdentifier Asset(AssetId asset, int64_t localIdentifierInFile, std::string filePath = {})
        {
            ObjectIdentifier identifier;
            identifier.localIdentifierInFile = localIdentifierInFile;
            identifier.guid = asset.GetGuid();
            identifier.fileType = FileType::SerializedAssetType;
            identifier.filePath = std::move(filePath);
            return identifier;
        }

        bool IsValid() const
        {
            return IsLocalObject() || IsAsset();
        }

        bool IsLocalObject() const
        {
            return localIdentifierInFile != 0 &&
                   !guid.IsValid() &&
                   fileType == FileType::NonAssetType &&
                   filePath.empty();
        }

        bool IsAsset() const
        {
            return localIdentifierInFile != 0 &&
                   guid.IsValid() &&
                   IsKnownFileType(fileType) &&
                   fileType != FileType::NonAssetType;
        }

        AssetId ToAssetId() const
        {
            return AssetId(guid);
        }

        SerializedObjectIdentifier ToSerializedObjectIdentifier() const
        {
            return {guid.IsValid() ? 1 : 0, localIdentifierInFile};
        }

        friend bool operator==(const ObjectIdentifier& lhs, const ObjectIdentifier& rhs)
        {
            return lhs.guid == rhs.guid &&
                   lhs.localIdentifierInFile == rhs.localIdentifierInFile &&
                   lhs.fileType == rhs.fileType;
        }
    };

    class NLS_ENGINE_API PersistentManager
    {
    public:
        static PersistentManager& Instance();

        void Clear();
        InstanceID RegisterObjectIdentifier(const ObjectIdentifier& identifier);
        void RegisterInstanceID(InstanceID instanceID, const ObjectIdentifier& identifier);
        InstanceID BindObjectIdentifier(NLS::Object& object, const ObjectIdentifier& identifier);
        bool InstanceIDToObjectIdentifier(InstanceID instanceID, ObjectIdentifier& identifier) const;
        bool InstanceIDToSerializedObjectIdentifier(InstanceID instanceID, SerializedObjectIdentifier& identifier) const;
        InstanceID ObjectIdentifierToInstanceID(const ObjectIdentifier& identifier);
        void InstanceIDToLocalSerializedObjectIdentifier(InstanceID instanceID, LocalSerializedObjectIdentifier& identifier) const;
        void LocalSerializedObjectIdentifierToInstanceID(const LocalSerializedObjectIdentifier& identifier, InstanceID& instanceID);

    private:
        struct FileIdentifierHash
        {
            size_t operator()(const FileIdentifier& identifier) const noexcept
            {
                size_t hash = std::hash<NLS::Guid> {}(identifier.guid);
                hash ^= std::hash<int32_t> {}(static_cast<int32_t>(identifier.type)) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

        struct ObjectIdentifierHash
        {
            size_t operator()(const ObjectIdentifier& identifier) const noexcept
            {
                size_t hash = std::hash<int64_t> {}(identifier.localIdentifierInFile);
                hash ^= std::hash<NLS::Guid> {}(identifier.guid) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
                hash ^= std::hash<int32_t> {}(static_cast<int32_t>(identifier.fileType)) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

        struct ObjectIdentifierIdentityEqual
        {
            bool operator()(const ObjectIdentifier& lhs, const ObjectIdentifier& rhs) const noexcept
            {
                return lhs.localIdentifierInFile == rhs.localIdentifierInFile &&
                       lhs.guid == rhs.guid &&
                       lhs.fileType == rhs.fileType;
            }
        };

        static bool SamePersistentIdentity(const ObjectIdentifier& lhs, const ObjectIdentifier& rhs);
        static FileIdentifier MakeFileIdentifier(const ObjectIdentifier& identifier);
        void RefreshPathHintLocked(InstanceID instanceID, const ObjectIdentifier& hint);
        void RegisterExternalFileLocked(const ObjectIdentifier& identifier);
        void ReleaseReservedInstanceIDIfOrphanedLocked(InstanceID instanceID) const;
        void EraseInstanceMappingLocked(InstanceID instanceID, bool releaseReservedInstanceID);
        int32_t GetOrCreateExternalFileIndexLocked(const ObjectIdentifier& identifier) const;

        std::unordered_map<InstanceID, ObjectIdentifier> m_identifierByInstanceID;
        std::unordered_map<ObjectIdentifier, InstanceID, ObjectIdentifierHash, ObjectIdentifierIdentityEqual> m_instanceIDByIdentifier;
        mutable std::unordered_map<FileIdentifier, int32_t, FileIdentifierHash> m_serializedFileIndexByFileIdentifier;
        mutable std::unordered_map<int32_t, FileIdentifier> m_fileIdentifierBySerializedFileIndex;
        mutable int32_t m_nextSerializedFileIndex = 1;
        mutable std::mutex m_mutex;
    };

    inline void InstanceIDToLocalSerializedObjectIdentifier(InstanceID id, LocalSerializedObjectIdentifier& localIdentifier)
    {
        PersistentManager::Instance().InstanceIDToLocalSerializedObjectIdentifier(id, localIdentifier);
    }

    inline void LocalSerializedObjectIdentifierToInstanceID(const LocalSerializedObjectIdentifier& localIdentifier, InstanceID& memoryID)
    {
        PersistentManager::Instance().LocalSerializedObjectIdentifierToInstanceID(localIdentifier, memoryID);
    }

    namespace Detail
    {
        template <typename T, typename = void>
        struct IsCompleteObjectTarget : std::false_type
        {
        };

        template <typename T>
        struct IsCompleteObjectTarget<T, std::void_t<decltype(sizeof(std::remove_cv_t<T>))>>
            : std::bool_constant<std::is_base_of_v<NLS::Object, std::remove_cv_t<T>>>
        {
        };

        template <typename T>
        inline constexpr bool IsCompleteObjectTargetV = IsCompleteObjectTarget<T>::value;
    }

    template <typename T>
        requires Detail::IsCompleteObjectTargetV<T>
    class PPtr
    {
    public:
        PPtr() = default;
        explicit PPtr(InstanceID instanceID)
            : m_instanceID(instanceID)
        {
        }
        explicit PPtr(const T* object)
        {
            AssignObject(object);
        }

        InstanceID GetInstanceID() const
        {
            return m_instanceID;
        }

        void SetInstanceID(InstanceID instanceID)
        {
            m_instanceID = instanceID;
        }

        void AssignObject(const T* object)
        {
            m_instanceID = object != nullptr ? object->GetInstanceID() : InstanceID_None;
        }

        PPtr& operator=(const T* object)
        {
            AssignObject(object);
            return *this;
        }

        bool IsNull() const
        {
            return m_instanceID == InstanceID_None;
        }

        bool IsValid() const
        {
            return Get() != nullptr;
        }

        T* Get() const
        {
            return NLS::Object::IDToPointer<std::remove_cv_t<T>>(m_instanceID);
        }

        operator T*() const
        {
            return Get();
        }

        T* operator->() const
        {
            return Get();
        }

        T& operator*() const
        {
            return *Get();
        }

        bool operator==(const PPtr& other) const
        {
            return m_instanceID == other.m_instanceID;
        }

        bool operator!=(const PPtr& other) const
        {
            return !(*this == other);
        }

        bool operator<(const PPtr& other) const
        {
            return m_instanceID < other.m_instanceID;
        }

        static const char* StaticMetaTypeName()
        {
            static const std::string name = std::string("NLS::Engine::Serialize::PPtr<") + GetPPtrElementTypeName<T>() + ">";
            return name.c_str();
        }

        static NLS::meta::TypeKey StaticMetaTypeKey()
        {
            return NLS::meta::MakeTypeKey(StaticMetaTypeName());
        }

    private:
        template <typename U>
        static const char* GetPPtrElementTypeName()
        {
            using Clean = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<U>>>;
            if (const auto* name = NLS::meta::GetStaticTypeName<Clean>())
                return name;
            return typeid(Clean).name();
        }

        InstanceID m_instanceID = InstanceID_None;
    };
}
