#pragma once

#include <array>
#include <stdexcept>
#include <string>

#include "Reflection/Field.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/PPtr.h"
#include "Serialize/PPtrResourceTypes.h"

namespace NLS::Engine::Serialize
{
    namespace Internal
    {
        enum class PPtrApplyResult
        {
            Applied,
            ShapeMismatch
        };

        struct PPtrObjectGraphHandler
        {
            NLS::meta::Type valueType;
            PropertyValue (*serializeValue)(const NLS::meta::Variant& value);
            PPtrApplyResult (*applyValue)(
                NLS::meta::Variant& instance,
                const NLS::meta::Field& field,
                const PropertyValue& value);
            PPtrApplyResult (*applyArray)(
                NLS::meta::Variant& instance,
                const NLS::meta::Field& field,
                const PropertyValue& value);
        };

        inline bool IsPPtrTypeName(const NLS::meta::Type& type)
        {
            const auto name = type.GetName();
            return name.rfind("NLS::Engine::Serialize::PPtr<", 0) == 0;
        }

        inline ObjectIdentifier ToObjectIdentifier(const InstanceID instanceID)
        {
            if (instanceID == InstanceID_None)
                return {};

            ObjectIdentifier identifier;
            if (!PersistentManager::Instance().InstanceIDToObjectIdentifier(instanceID, identifier))
                // Unity-style transient object references can be visible at runtime but have no persistent fileID.
                return {};
            return identifier;
        }

        inline std::runtime_error UnsupportedPPtrTypeError(const NLS::meta::Type& type)
        {
            return std::runtime_error(
                "Unsupported reflected PPtr object reference type: " + type.GetName());
        }

        template <typename T>
        PropertyValue SerializePPtrValue(const NLS::meta::Variant& value)
        {
            static_assert(
                Detail::IsCompleteObjectTargetV<T>,
                "Reflected PPtr object references require Object-derived targets.");
            const auto& reference = value.GetValue<PPtr<T>>();
            return PropertyValue::ObjectReference(ToObjectIdentifier(reference.GetInstanceID()));
        }

        template <typename T>
        PPtrApplyResult ApplyPPtrValue(
            NLS::meta::Variant& instance,
            const NLS::meta::Field& field,
            const PropertyValue& value)
        {
            static_assert(
                Detail::IsCompleteObjectTargetV<T>,
                "Reflected PPtr object references require Object-derived targets.");
            if (value.GetKind() != PropertyValue::Kind::ObjectReference)
                return PPtrApplyResult::ShapeMismatch;

            PPtr<T> reference(
                PersistentManager::Instance().ObjectIdentifierToInstanceID(value.GetObjectReference()));
            field.SetValue(instance, NLS::meta::Variant(reference, NLS::meta::variant_policy::NoCopy {}));
            return PPtrApplyResult::Applied;
        }

        template <typename T>
        PPtrApplyResult ApplyPPtrArray(
            NLS::meta::Variant& instance,
            const NLS::meta::Field& field,
            const PropertyValue& value)
        {
            static_assert(
                Detail::IsCompleteObjectTargetV<T>,
                "Reflected PPtr object references require Object-derived targets.");
            if (value.GetKind() != PropertyValue::Kind::Array)
                return PPtrApplyResult::ShapeMismatch;

            NLS::Array<PPtr<T>> references;
            references.reserve(value.GetArray().size());
            for (const auto& item : value.GetArray())
            {
                if (item.GetKind() != PropertyValue::Kind::ObjectReference)
                    return PPtrApplyResult::ShapeMismatch;

                references.push_back(
                    PPtr<T>(
                        PersistentManager::Instance().ObjectIdentifierToInstanceID(item.GetObjectReference())));
            }

            field.SetValue(instance, NLS::meta::Variant(references, NLS::meta::variant_policy::NoCopy {}));
            return PPtrApplyResult::Applied;
        }

        template <typename T>
        PPtrObjectGraphHandler MakePPtrObjectGraphHandler()
        {
            static_assert(
                Detail::IsCompleteObjectTargetV<T>,
                "Reflected PPtr object references require Object-derived targets.");
            const auto valueType = NLS_TYPEOF(PPtr<T>);
            if (!valueType.IsValid())
                throw UnsupportedPPtrTypeError(NLS::meta::Type::Invalid());

            return {
                valueType,
                &SerializePPtrValue<T>,
                &ApplyPPtrValue<T>,
                &ApplyPPtrArray<T>
            };
        }

        inline const std::array<PPtrObjectGraphHandler, kPPtrResourceTargetCount>& GetPPtrObjectGraphHandlers()
        {
            static const std::array<PPtrObjectGraphHandler, kPPtrResourceTargetCount> handlers = {
#define NLS_ENGINE_SERIALIZE_MAKE_PPTR_OBJECT_GRAPH_HANDLER(type, label, artifactType, subAssetPrefix) MakePPtrObjectGraphHandler<type>(),
                NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_ENGINE_SERIALIZE_MAKE_PPTR_OBJECT_GRAPH_HANDLER)
#undef NLS_ENGINE_SERIALIZE_MAKE_PPTR_OBJECT_GRAPH_HANDLER
            };
            return handlers;
        }

        inline const PPtrObjectGraphHandler* FindPPtrObjectGraphHandler(const NLS::meta::Type& type)
        {
            for (const auto& handler : GetPPtrObjectGraphHandlers())
            {
                if (handler.valueType == type)
                    return &handler;
            }
            return nullptr;
        }

        inline PropertyValue SerializePPtrValueOrThrow(const NLS::meta::Variant& value)
        {
            const auto type = value.GetType();
            const auto* handler = FindPPtrObjectGraphHandler(type);
            if (handler == nullptr)
                throw UnsupportedPPtrTypeError(type);
            return handler->serializeValue(value);
        }

        inline PPtrApplyResult ApplyPPtrValueOrThrow(
            NLS::meta::Variant& instance,
            const NLS::meta::Field& field,
            const PropertyValue& value)
        {
            const auto fieldType = field.GetType();
            const auto* handler = FindPPtrObjectGraphHandler(fieldType);
            if (handler == nullptr)
                throw UnsupportedPPtrTypeError(fieldType);
            return handler->applyValue(instance, field, value);
        }

        inline PPtrApplyResult ApplyPPtrArrayOrThrow(
            NLS::meta::Variant& instance,
            const NLS::meta::Field& field,
            const PropertyValue& value)
        {
            const auto elementType = field.GetType().GetArrayType();
            const auto* handler = FindPPtrObjectGraphHandler(elementType);
            if (handler == nullptr)
                throw UnsupportedPPtrTypeError(elementType);
            return handler->applyArray(instance, field, value);
        }
    }
}
