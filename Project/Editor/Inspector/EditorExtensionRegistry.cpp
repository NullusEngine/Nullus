#include "Inspector/EditorExtensionRegistry.h"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "Reflection/TypeConfig.h"

namespace NLS::Editor::Inspector
{
namespace
{
struct DescriptorScanStore
{
    std::mutex mutex;
    std::unordered_map<uint64_t, std::unordered_set<std::string>> appliedFingerprints;
};

DescriptorScanStore& GetDescriptorScanStore()
{
    static DescriptorScanStore store;
    return store;
}

std::string TypeKeyText(const meta::TypeKey key)
{
    return std::to_string(key.high) + ":" + std::to_string(key.low);
}

std::string RegistrationKey(const EditorExtensionDescriptor& descriptor)
{
    return TypeKeyText(descriptor.inspectedTypeKey) + "|" +
        (descriptor.editorForChildClasses ? "children" : "exact") + "|" +
        std::to_string(descriptor.priority);
}

std::string DescriptorFingerprint(const EditorExtensionDescriptor& descriptor)
{
    return RegistrationKey(descriptor) + "|" + TypeKeyText(descriptor.editorTypeKey) + "|" +
        (descriptor.isFallback ? "fallback" : "primary") + "|" +
        (descriptor.supportsMultiObject ? "multi" : "single") + "|" +
        (descriptor.identifier != nullptr ? descriptor.identifier : "");
}

void ValidateDescriptors(const std::span<const EditorExtensionDescriptor> descriptors)
{
    std::unordered_set<std::string> registrationKeys;
    for (const auto& descriptor : descriptors)
    {
        if (descriptor.kind != EditorExtensionKind::ComponentEditor)
            throw std::invalid_argument("Unsupported Inspector editor extension kind.");
        if (descriptor.inspectedTypeKey == meta::InvalidTypeKey ||
            descriptor.editorTypeKey == meta::InvalidTypeKey)
        {
            throw std::invalid_argument("Inspector editor extension descriptor has an invalid TypeKey.");
        }
        if (descriptor.inspectedTypeName == nullptr || descriptor.editorTypeName == nullptr ||
            descriptor.create == nullptr)
        {
            throw std::invalid_argument("Inspector editor extension descriptor is incomplete.");
        }

        const std::string key = RegistrationKey(descriptor);
        if (!registrationKeys.insert(key).second)
            throw std::logic_error(
                "Duplicate Inspector component editor descriptor for target/inheritance/priority: " + key);
    }
}

ComponentEditorRegistration MakeRegistration(const EditorExtensionDescriptor& descriptor)
{
    const auto inspectedType = meta::ResolveTypeByKey(descriptor.inspectedTypeKey, false);
    const auto editorType = meta::ResolveTypeByKey(descriptor.editorTypeKey, false);
    if (!inspectedType.IsValid())
        throw std::logic_error(std::string("Inspector editor target reflection type is unavailable: ") + descriptor.inspectedTypeName);
    if (!editorType.IsValid())
        throw std::logic_error(std::string("Inspector editor reflection type is unavailable: ") + descriptor.editorTypeName);

    return {
        inspectedType,
        descriptor.identifier != nullptr ? descriptor.identifier : descriptor.editorTypeName,
        descriptor.editorForChildClasses,
        descriptor.isFallback,
        descriptor.supportsMultiObject,
        descriptor.create,
        descriptor.priority};
}
} // namespace

void RegisterEditorExtensionDescriptors(
    InspectorRegistry& registry,
    const std::span<const EditorExtensionDescriptor> descriptors)
{
    ValidateDescriptors(descriptors);

    auto& store = GetDescriptorScanStore();
    std::scoped_lock lock(store.mutex);
    auto& applied = store.appliedFingerprints[registry.GetLifecycleId()];
    for (const auto& descriptor : descriptors)
    {
        const auto fingerprint = DescriptorFingerprint(descriptor);
        if (applied.contains(fingerprint))
            continue;

        registry.RegisterComponentEditor(MakeRegistration(descriptor));
        applied.insert(fingerprint);
    }
}
} // namespace NLS::Editor::Inspector
