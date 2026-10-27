#include "Inspector/InspectorRegistry.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace NLS::Editor::Inspector
{
namespace
{
std::atomic<uint64_t> g_nextInspectorRegistryLifecycleId {1u};

bool MatchesMetadataKey(const PropertyNode& node, const std::string& key)
{
    if (!node.metadata.customDrawer.empty() && node.metadata.customDrawer == key)
        return true;
    if (key == "Flags") return node.metadata.flags;
    if (key == "Multiline") return node.metadata.multiline;
    if (key == "ColorUsage") return node.metadata.colorUsage;
    if (key == "Range") return node.metadata.rangeMin.has_value() && node.metadata.rangeMax.has_value();
    if (key == "Min") return node.metadata.min.has_value();
    if (key == "Max") return node.metadata.max.has_value();
    if (key == "ReadOnly") return node.readOnly || node.metadata.readOnly;
    if (key == "HideInInspector") return node.metadata.hideInInspector;
    if (key == "Header") return !node.metadata.header.empty();
    if (key == "Space") return node.metadata.spaceBefore > 0.0f;
    if (key == "Tooltip") return !node.metadata.tooltip.empty();
    if (key == "ReferenceTypeConstraint") return !node.metadata.referenceTypeConstraint.empty();
    return false;
}
} // namespace

InspectorRegistry::InspectorRegistry()
    : m_lifecycleId(g_nextInspectorRegistryLifecycleId.fetch_add(1u, std::memory_order_relaxed))
{
}

InspectorRegistry& InspectorRegistry::Get()
{
    static InspectorRegistry registry;
    return registry;
}

void InspectorRegistry::RegisterTargetAdapter(std::shared_ptr<IInspectorTargetAdapter> adapter)
{
    if (!adapter)
        throw std::invalid_argument("Inspector target adapter must not be null.");
    m_adapters.push_back(std::move(adapter));
    ++m_version;
}

const IInspectorTargetAdapter* InspectorRegistry::ResolveTargetAdapter(const InspectorTargetSet& targets) const
{
    const auto found = std::find_if(m_adapters.begin(), m_adapters.end(), [&](const auto& adapter)
    {
        return adapter && adapter->CanBuild(targets);
    });
    return found != m_adapters.end() ? found->get() : nullptr;
}

void InspectorRegistry::RegisterTargetEditor(TargetEditorRegistration registration)
{
    const auto duplicate = std::find_if(m_targetEditors.begin(), m_targetEditors.end(), [&](const auto& existing)
    {
        return existing.targetKind == registration.targetKind;
    });
    if (duplicate != m_targetEditors.end())
        throw std::logic_error("Duplicate Inspector target editor registration: " + registration.identifier);
    m_targetEditors.push_back(std::move(registration));
    ++m_version;
}

void InspectorRegistry::RegisterComponentEditor(ComponentEditorRegistration registration)
{
    const auto duplicate = std::find_if(m_componentEditors.begin(), m_componentEditors.end(), [&](const auto& existing)
    {
        return existing.inspectedType == registration.inspectedType &&
            existing.editorForChildClasses == registration.editorForChildClasses &&
            existing.priority == registration.priority;
    });
    if (duplicate != m_componentEditors.end())
        throw std::logic_error("Duplicate Inspector component editor registration: " + registration.identifier);
    if (!registration.inspectedType.IsValid())
        throw std::invalid_argument("Inspector component editor type must be valid: " + registration.identifier);
    if (!registration.create)
        throw std::invalid_argument("Inspector component editor factory must not be empty: " + registration.identifier);
    m_componentEditors.push_back(std::move(registration));
    ++m_version;
}

void InspectorRegistry::UnregisterComponentEditorsByIdentifierPrefix(std::string_view prefix)
{
    const auto removed = std::erase_if(m_componentEditors, [&](const auto& registration)
    {
        return prefix.empty() || registration.identifier.rfind(prefix, 0) == 0;
    });
    if (removed > 0)
        ++m_version;
}

void InspectorRegistry::RegisterPropertyDrawer(PropertyDrawerRegistration registration)
{
    if(!registration.identifier.empty() && std::any_of(m_propertyDrawers.begin(),m_propertyDrawers.end(),[&](const auto& entry){return entry.identifier==registration.identifier;}))
        throw std::logic_error("Duplicate property drawer identifier: "+registration.identifier);
    if (IsDuplicate(registration))
        throw std::logic_error("Duplicate Inspector property drawer registration: " + registration.identifier);
    m_propertyDrawers.push_back(std::move(registration));
    ++m_version;
}

bool InspectorRegistry::UnregisterPropertyDrawer(std::string_view identifier)
{
    if(std::none_of(m_propertyDrawers.begin(),m_propertyDrawers.end(),[&](const auto& entry){return entry.identifier==identifier;})) return false;
    // Destroy extension UI state while its defining module is still loaded.
    const auto listeners=m_drawerListeners;
    for(const auto& [id,listener]:listeners) if(listener) listener();
    const auto removed = std::erase_if(m_propertyDrawers, [&](const auto& entry) { return entry.identifier == identifier; });
    if (removed) ++m_version;
    return removed != 0;
}

uint64_t InspectorRegistry::AddPropertyDrawerInvalidationListener(std::function<void()> listener)
{
    const auto id=m_nextDrawerListener++;m_drawerListeners.emplace(id,std::move(listener));return id;
}
void InspectorRegistry::RemovePropertyDrawerInvalidationListener(uint64_t id) {m_drawerListeners.erase(id);}

void InspectorRegistry::RegisterDecoratorDrawer(DecoratorDrawerRegistration registration)
{
    const auto duplicate = std::find_if(m_decoratorDrawers.begin(), m_decoratorDrawers.end(), [&](const auto& existing)
    {
        return existing.metadataKey == registration.metadataKey;
    });
    if (duplicate != m_decoratorDrawers.end())
        throw std::logic_error("Duplicate Inspector decorator drawer registration: " + registration.identifier);
    m_decoratorDrawers.push_back(std::move(registration));
    ++m_version;
}

std::optional<PropertyDrawerRegistration> InspectorRegistry::ResolvePropertyDrawer(const PropertyNode& node) const
{
    const auto findScope = [&](const InspectorDrawerScope scope, const auto& predicate) -> std::optional<PropertyDrawerRegistration>
    {
        const auto found = std::find_if(m_propertyDrawers.begin(), m_propertyDrawers.end(), [&](const auto& registration)
        {
            return registration.scope == scope && predicate(registration);
        });
        return found != m_propertyDrawers.end() ? std::optional<PropertyDrawerRegistration>(*found) : std::nullopt;
    };

    if (const auto field = findScope(InspectorDrawerScope::Field, [&](const auto& registration)
        { return registration.fieldPath == node.path && !registration.ownerTypeName.empty() && registration.ownerTypeName == node.ownerTypeName; }))
        return field;
    if (const auto field = findScope(InspectorDrawerScope::Field, [&](const auto& registration)
        { return registration.fieldPath == node.path && registration.ownerTypeName.empty(); }))
        return field;
    // A named drawer is an explicit field choice; broad Flags/Range/etc.
    // metadata handlers must not shadow it because they registered earlier.
    if(node.kind!=InspectorPropertyKind::Array && !node.metadata.customDrawer.empty())
        if(const auto named=findScope(InspectorDrawerScope::ExplicitMetadata,[&](const auto& registration)
            {return registration.metadataKey==node.metadata.customDrawer;}))
            return named;
    if (const auto metadata = findScope(InspectorDrawerScope::ExplicitMetadata, [&](const auto& registration)
        {
            return node.kind!=InspectorPropertyKind::Array && MatchesMetadataKey(node, registration.metadataKey);
        }))
        return metadata;
    if (const auto exact = findScope(InspectorDrawerScope::ExactType, [&](const auto& registration)
        { return registration.valueType == node.valueType; }))
        return exact;
    if (const auto family = findScope(InspectorDrawerScope::TypeFamily, [&](const auto& registration)
        { return registration.valueType.IsValid() && node.valueType.IsValid() && node.valueType.DerivesFrom(registration.valueType); }))
        return family;
    if (const auto builtin = findScope(InspectorDrawerScope::Builtin, [&](const auto& registration)
        { return registration.propertyKind == node.kind; }))
        return builtin;
    return findScope(InspectorDrawerScope::ReadOnlyFallback, [](const auto&) { return true; });
}

std::vector<DecoratorDrawerRegistration> InspectorRegistry::ResolveDecoratorDrawers(const PropertyNode& node) const
{
    std::vector<DecoratorDrawerRegistration> result;
    for (const auto& registration : m_decoratorDrawers)
    {
        if (MatchesMetadataKey(node, registration.metadataKey))
            result.push_back(registration);
    }
    return result;
}

std::optional<PropertyDrawerRegistration> InspectorRegistry::ResolveBuiltinPropertyDrawer(const PropertyNode& node) const
{
    const auto found=std::find_if(m_propertyDrawers.begin(),m_propertyDrawers.end(),[&](const auto& entry)
    {return entry.scope==InspectorDrawerScope::Builtin && entry.propertyKind==node.kind;});
    return found==m_propertyDrawers.end()?std::nullopt:std::optional<PropertyDrawerRegistration>(*found);
}

std::optional<ComponentEditorRegistration> InspectorRegistry::ResolveComponentEditor(const meta::Type& type) const
{
    const auto matches = [&](const ComponentEditorRegistration& registration)
    {
        // Script-targeted editors never match on the shared ScriptComponent
        // reflection type; they are resolved by ResolveScriptComponentEditor.
        if (!registration.scriptTypeName.empty())
            return false;
        if (registration.inspectedType == type)
            return true;
        return registration.editorForChildClasses && type.IsValid() && registration.inspectedType.IsValid() &&
            type.DerivesFrom(registration.inspectedType);
    };

    const auto resolve = [&](const bool fallback, const bool exact) -> std::optional<ComponentEditorRegistration>
    {
        std::optional<ComponentEditorRegistration> best;
        for (const auto& registration : m_componentEditors)
        {
            const bool isExact = registration.inspectedType == type;
            if (registration.isFallback != fallback || !matches(registration) ||
                (exact ? !isExact : isExact))
            {
                continue;
            }
            if (!best.has_value() || registration.priority > best->priority ||
                (registration.priority == best->priority && registration.identifier < best->identifier))
            {
                best = registration;
            }
        }
        return best;
    };

    if (const auto exact = resolve(false, true))
        return exact;
    if (const auto child = resolve(false, false))
        return child;
    if (const auto exactFallback = resolve(true, true))
        return exactFallback;
    return resolve(true, false);
}

std::optional<ComponentEditorRegistration> InspectorRegistry::ResolveScriptComponentEditor(
    std::string_view scriptTypeName) const
{
    if (scriptTypeName.empty())
        return std::nullopt;

    std::optional<ComponentEditorRegistration> best;
    for (const auto& registration : m_componentEditors)
    {
        if (registration.scriptTypeName != scriptTypeName || registration.isFallback)
            continue;
        if (!best.has_value() || registration.priority > best->priority ||
            (registration.priority == best->priority && registration.identifier < best->identifier))
        {
            best = registration;
        }
    }
    return best;
}

uint64_t InspectorRegistry::GetVersion() const
{
    return m_version;
}

uint64_t InspectorRegistry::GetLifecycleId() const
{
    return m_lifecycleId;
}

bool InspectorRegistry::IsDuplicate(const PropertyDrawerRegistration& registration) const
{
    return std::any_of(m_propertyDrawers.begin(), m_propertyDrawers.end(), [&](const auto& existing)
    {
        return existing.scope == registration.scope &&
            existing.fieldPath == registration.fieldPath &&
            existing.metadataKey == registration.metadataKey &&
            existing.valueType == registration.valueType &&
            existing.ownerTypeName == registration.ownerTypeName &&
            existing.propertyKind == registration.propertyKind;
    });
}
} // namespace NLS::Editor::Inspector
