#include "Inspector/ComponentEditor.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "Serialization/AccessorSerializedBackend.h"
#include "Serialization/NativeReflectionSerializedBackend.h"

#include <Scripting/ScriptComponent.h>

namespace NLS::Editor::Inspector
{
namespace
{
std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}
} // namespace


ComponentEditorContext::ComponentEditorContext(
    Serialization::SerializedObject& serializedObject,
    InspectorTargetSet targets,
    const std::string_view searchText,
    ComponentEditorContextCallbacks callbacks)
    : m_serializedObject(serializedObject)
    , m_targets(std::move(targets))
    , m_searchText(searchText)
    , m_callbacks(std::move(callbacks))
{
    m_targets.Normalize();
}

ComponentEditorContext::~ComponentEditorContext() = default;

bool ComponentEditorContext::DrawDefaultInspector()
{
    bool drawn = false;
    auto iterator = m_serializedObject.GetIterator();
    const auto* stream = m_serializedObject.GetPrimaryByteStream();
    const auto schema = stream ? stream->GetSchema() : nullptr;
    const auto rootPath = schema ? schema->GetPath(schema->GetRoot()) : std::string{};
    const auto fieldDepth = (iterator ? iterator.GetDepth() : 0u) + (rootPath.empty() ? 0u : 1u);
    bool enterChildren = true;
    while (iterator && iterator.NextVisible(enterChildren))
    {
        // Named roots (native components) and unnamed roots (accessor/script
        // backends) are serialization containers. Draw their direct fields;
        // each PropertyField owns any nested structures and array elements.
        if (!rootPath.empty() && iterator.GetPropertyPath() == rootPath)
        {
            enterChildren = true;
            continue;
        }
        enterChildren = false;
        if (iterator.GetDepth() == fieldDepth)
        {
            drawn = PropertyField(iterator, ComponentEditorPropertyOptions{.forceVisible = false}) || drawn;
        }
    }
    return drawn;
}

bool ComponentEditorContext::PropertyField(
    const Serialization::SerializedProperty& property,
    ComponentEditorPropertyOptions options)
{
    if (!property || !MatchesSearch(property))
        return false;

    return m_callbacks.drawProperty
        ? m_callbacks.drawProperty(property, options)
        : false;
}

bool ComponentEditorContext::PropertyField(
    const std::string_view propertyPath,
    ComponentEditorPropertyOptions options)
{
    return PropertyField(m_serializedObject.FindProperty(propertyPath), std::move(options));
}

bool ComponentEditorContext::Button(
    std::string label,
    std::function<void()> execute,
    const bool enabled,
    std::string tooltip,
    std::string searchText)
{
    if (searchText.empty())
        searchText = label;
    if (!MatchesSearch(label, searchText, {}, m_searchText))
        return false;

    ComponentEditorAction action {
        std::move(label),
        std::move(tooltip),
        std::move(searchText),
        enabled,
        std::move(execute)};
    return m_callbacks.drawButton ? m_callbacks.drawButton(action) : false;
}

void ComponentEditorContext::HelpBox(std::string text, const ComponentEditorMessageKind kind)
{
    if (m_callbacks.drawHelpBox)
        m_callbacks.drawHelpBox({kind, std::move(text)});
}

bool ComponentEditorContext::ApplyModifiedProperties()
{
    if (m_callbacks.applyModifiedProperties)
        return m_callbacks.applyModifiedProperties();
    return m_serializedObject.ApplyModifiedProperties();
}

bool ComponentEditorContext::DiscardModifiedProperties()
{
    if (m_callbacks.discardModifiedProperties)
        return m_callbacks.discardModifiedProperties();
    const bool changed = m_serializedObject.HasModifiedProperties();
    m_serializedObject.DiscardModifiedProperties();
    return changed;
}

bool ComponentEditorContext::MatchesSearch(
    const std::string_view displayName,
    const std::string_view fieldName,
    const std::string_view path,
    const std::string_view searchText)
{
    if (searchText.empty())
        return true;

    const auto needle = LowerAscii(std::string(searchText));
    return LowerAscii(std::string(displayName)).find(needle) != std::string::npos ||
        LowerAscii(std::string(fieldName)).find(needle) != std::string::npos ||
        LowerAscii(std::string(path)).find(needle) != std::string::npos;
}

bool ComponentEditorContext::MatchesSearch(const Serialization::SerializedProperty& property) const
{
    return MatchesSearch(
        property.GetDisplayName(),
        property.GetName(),
        property.GetPropertyPath(),
        m_searchText) ||
        LowerAscii(std::string(property.GetTooltip())).find(LowerAscii(m_searchText)) != std::string::npos;
}

void ComponentEditor::UpdateSerializedObject(const PropertyNode& componentNode)
{
    Serialization::SerializedTargetSet targets;
    targets.targets = GetPropertyNodeTargets(componentNode);
    if (targets.targets.empty() && componentNode.target.IsValid())
        targets.targets = {componentNode.target};
    targets.Normalize();

    const bool useAccessorStream = Serialization::AccessorSerializedBackend::Supports(targets);
    const bool nativeReflectionTargets = !useAccessorStream && !targets.Empty() && std::all_of(
        targets.targets.begin(), targets.targets.end(), [](const InspectorTargetHandle& target)
        {
            const auto* object = Serialization::NativeReflectionSerializedBackend::Resolve(target);
            return object != nullptr;
        });
    const auto backendKind = useAccessorStream
        ? BoundBackendKind::Accessor
        : (nativeReflectionTargets ? BoundBackendKind::NativeReflection : BoundBackendKind::None);

    // Refresh values for a stable binding. Reset also clears expansion state
    // and local structure revisions, so it belongs to a target/backend change.
    // Keep staged previews until the control commits or cancels its gesture.
    if (m_boundBackend == backendKind && m_boundTargets == targets && m_serializedObject.IsValid())
    {
        if (!m_serializedObject.HasModifiedProperties())
            m_serializedObject.Update();
        return;
    }

    m_boundTargets = targets;
    m_boundBackend = backendKind;

    // Getter/setter-backed component state has a native byte-stream backend of
    // its own. Do not reconstruct an Inspector presentation merely to reach
    // those properties: UI Canvas shares the same SerializedObject contract as
    // reflected components.
    if (useAccessorStream)
    {
        m_serializedObject.Reset(
            std::move(targets),
            Serialization::AccessorSerializedBackend::Create());
        return;
    }

    // Ordinary native components already have a reflected byte-stream codec.
    // Their custom editors can therefore use the same canonical snapshot as
    // managed callers without going through a UI binding bridge.
    // Script components are handled above by AccessorSerializedBackend, whose
    // TypeTree is built from ScriptFieldDescriptor.
    if (nativeReflectionTargets)
    {
        auto backend = Serialization::NativeReflectionSerializedBackend::Create();
        // Unsupported reflected layouts stay unavailable until their codec is
        // registered; there is no getter/setter bridge fallback.
        if (backend->BuildTypeTree(targets, Serialization::SerializedInspectorMode::Normal).Succeeded())
        {
            m_serializedObject.Reset(std::move(targets), std::move(backend));
            return;
        }
    }

    m_serializedObject.Reset({}, {});
}

const InspectorTargetSet& ComponentEditor::targets() const
{
    static const InspectorTargetSet empty;
    return m_context != nullptr ? m_context->GetTargets() : empty;
}

const InspectorTargetHandle* ComponentEditor::target() const
{
    return m_context != nullptr ? m_context->GetTarget() : nullptr;
}

std::string_view ComponentEditor::SearchText() const
{
    return m_context != nullptr ? m_context->GetSearchText() : std::string_view {};
}

bool ComponentEditor::DrawDefaultInspector()
{
    return m_context != nullptr && m_context->DrawDefaultInspector();
}

bool ComponentEditor::PropertyField(
    const Serialization::SerializedProperty& property,
    ComponentEditorPropertyOptions options)
{
    return m_context != nullptr && m_context->PropertyField(property, std::move(options));
}

bool ComponentEditor::PropertyField(
    const std::string_view propertyPath,
    ComponentEditorPropertyOptions options)
{
    return m_context != nullptr && m_context->PropertyField(propertyPath, std::move(options));
}

bool ComponentEditor::Button(
    std::string label,
    std::function<void()> execute,
    const bool enabled,
    std::string tooltip,
    std::string searchText)
{
    return m_context != nullptr && m_context->Button(
        std::move(label),
        std::move(execute),
        enabled,
        std::move(tooltip),
        std::move(searchText));
}

void ComponentEditor::HelpBox(std::string text, const ComponentEditorMessageKind kind)
{
    if (m_context != nullptr)
        m_context->HelpBox(std::move(text), kind);
}

bool ComponentEditor::ApplyModifiedProperties()
{
    return m_context != nullptr && m_context->ApplyModifiedProperties();
}

bool ComponentEditor::DiscardModifiedProperties()
{
    return m_context != nullptr && m_context->DiscardModifiedProperties();
}

void DefaultComponentEditor::OnInspectorGUI()
{
    DrawDefaultInspector();
}
} // namespace NLS::Editor::Inspector
