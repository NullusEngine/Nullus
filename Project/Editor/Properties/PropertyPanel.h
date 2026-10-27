#pragma once
#include "Inspector/ReflectedPropertyEditor.h"
#include "Inspector/InspectorProjectionBuilder.h"

namespace NLS::Editor::Properties
{
struct PropertyPanelOptions
{
    std::string identity;
    std::string search;
    Inspector::PropertyEditorContext context;
};

// All hosts use the same renderer, property cursors and edit boundaries. The
// value overloads adapt owned reflected copies through the existing backend.
class PropertyPanel final
{
public:
    PropertyPanel();
    ~PropertyPanel();
    PropertyPanel(const PropertyPanel&) = delete;
    PropertyPanel& operator=(const PropertyPanel&) = delete;
    Inspector::PropertyDrawResult Draw(Serialization::SerializedObject& object,
                                       const PropertyPanelOptions& options = {});
    Inspector::ReflectedPropertyEdit DrawValue(std::string_view id, std::string_view label, meta::Variant& value,
                                               const Inspector::ReflectedPropertyOptions& options = {});
    template<class T>
    Inspector::ReflectedPropertyEdit DrawValue(std::string_view id, std::string_view label, T& value,
                                               const Inspector::ReflectedPropertyOptions& options = {})
    {
        return m_values.Draw(id, label, value, options);
    }
    template<class T>
    Inspector::ReflectedPropertyEdit Draw(std::string_view id, std::string_view label, T& value,
                                          const Inspector::ReflectedPropertyOptions& options = {})
    {
        return DrawValue(id, label, value, options);
    }
    Inspector::ReflectedPropertyEdit DrawValues(std::string_view id, std::string_view label,
                                                std::span<meta::Variant> values,
                                                const Inspector::ReflectedPropertyOptions& options = {})
    {
        return m_values.DrawValues(id, label, values, options);
    }
    template<class T>
    Inspector::ReflectedPropertyEdit DrawValues(std::string_view id, std::string_view label, std::vector<T>& values,
                                                const Inspector::ReflectedPropertyOptions& options = {})
    {
        return m_values.DrawValues(id, label, values, options);
    }
    void SetLifetime(std::weak_ptr<const void> lifetime)
    {
        m_values.SetLifetime(std::move(lifetime));
    }
    bool TakeCanceledEdits() { return m_values.TakeCanceledEdits(); }
    bool FinishFrame()
    {
        return m_values.FinishFrame();
    }
    void EndEdit()
    {
        m_values.EndEdit();
    }
    // Existing typed domain helpers keep their source-compatible Inspector API.
    operator Inspector::ReflectedPropertyEditor&()
    {
        return m_values;
    }
    void CancelEdits();
    bool HasActiveControls() const
    {
        return m_values.HasActiveControls() || m_renderer.HasActiveControls();
    }

    // Compatibility entry points used by Inspector custom editors.
    Inspector::PropertyDrawResult DrawProperties(const std::vector<Inspector::PropertyNode>& nodes,
                                                 const Inspector::PropertyEditorContext& context)
    {
        return m_renderer.DrawProperties(nodes, context);
    }
    Inspector::PropertyDrawResult DrawProperty(
        const Inspector::PropertyNode& node, const Inspector::PropertyEditorContext& context,
        Inspector::PropertyEditorLayout layout = Inspector::PropertyEditorLayout::ValueOnly)
    {
        return m_renderer.DrawProperty(node, context, layout);
    }
    Inspector::PropertyDrawResult FinishItem(const Inspector::PropertyNode& node,
                                             const Inspector::PropertyEditorContext& context)
    {
        return m_renderer.FinishItem(node, context);
    }
    const std::string& GetLastError() const
    {
        return m_values.GetLastError().empty() ? m_renderer.GetLastError() : m_values.GetLastError();
    }

private:
    std::shared_ptr<const void> m_lifetime;
    Inspector::InspectorPropertyEditor m_renderer;
    Inspector::ReflectedPropertyEditor m_values;
    std::vector<Inspector::PropertyNode> m_projection;
    Serialization::SerializedVersionStamp m_version;
    std::string m_identity, m_search;
    uint64_t m_registryVersion = 0, m_metadataVersion = 0;
};
} // namespace NLS::Editor::Properties
