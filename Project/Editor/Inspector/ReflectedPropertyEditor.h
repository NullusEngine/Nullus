#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Inspector/InspectorTypes.h"
#include "Inspector/InspectorPropertyEditor.h"
#include "Serialization/SerializedObject.h"

namespace NLS::Editor::Inspector
{
struct ReflectedPropertyOptions
{
    std::vector<std::string> hidden, disabled;
    std::vector<InspectorPropertyChoice> choices;
    std::optional<EditorPropertyMetadata> metadata;
    PropertyEditorLayout layout = PropertyEditorLayout::Properties;
    // Invoked synchronously; presentation caches never retain host callbacks.
    std::function<std::optional<meta::Variant>(std::string_view)> defaultValue, inheritedValue;
    std::function<bool()> restoreInherited;

};

struct ReflectedPropertyEdit
{
    bool changed = false, completed = false, canceled = false;
    std::vector<std::string> paths;
    bool Changed(std::string_view field) const;
};

// The backend owns one or more reflected editing values, never a pointer into
// an asset. Multiple values are exposed as multiple SerializedObject targets so
// the shared Inspector can show per-field mixed values; an edit writes the same
// value into every target. Hosts commit through their own document transaction.
class ReflectedValueBackend final : public Serialization::ISerializedTargetBackend
{
public:
    ReflectedValueBackend(meta::Variant value,std::weak_ptr<const void> lifetime,
        ReflectedPropertyOptions options = {});
    ReflectedValueBackend(std::vector<meta::Variant> values,std::weak_ptr<const void> lifetime,
        ReflectedPropertyOptions options = {});
    bool Alive() const;
    void SetSource(meta::Variant value);
    void SetSources(std::vector<meta::Variant> values);
    // Primary value (first target). Empty backends expose an invalid Variant.
    const meta::Variant& Value() const;
    [[nodiscard]] const std::vector<meta::Variant>& Values() const { return m_values; }
    Engine::Serialize::TypeTreeResult BuildTypeTree(const Serialization::SerializedTargetSet&,
        Serialization::SerializedInspectorMode) const override;
    Engine::Serialize::StreamResult SerializeTarget(const Serialization::SerializedTargetHandle&,
        std::shared_ptr<const Engine::Serialize::SerializedTypeTree>,
        Engine::Serialize::SerializedReferenceTable&) const override;
    Serialization::ValidationResult ValidateAndNormalize(const Serialization::SerializedTargetHandle&,
        std::span<const Serialization::SerializedPropertyPath>,Engine::Serialize::SerializedByteStream&) const override;
    Serialization::ApplyResult DeserializeTarget(const Serialization::SerializedTargetHandle&,
        const Engine::Serialize::SerializedByteStream&,const Undo::UndoExecutionContext&) const override;
    Serialization::SerializedVersionStamp GetVersion(const Serialization::SerializedTargetHandle&) const override;
private:
    // Multi-target handles carry their target index in assetId ("0", "1", ...).
    [[nodiscard]] std::size_t TargetIndex(const Serialization::SerializedTargetHandle& target) const;
    mutable std::vector<meta::Variant> m_values;
    std::weak_ptr<const void> m_lifetime;
    ReflectedPropertyOptions m_options;
    mutable uint64_t m_revision = 1;
};

class ReflectedPropertyEditor final
{
public:
    ReflectedPropertyEditor();
    ~ReflectedPropertyEditor();
    void SetLifetime(std::weak_ptr<const void> lifetime);
    bool FinishFrame();
    void EndEdit();
    void CancelEdits();
    bool TakeCanceledEdits();
    [[nodiscard]] bool HasActiveControls() const;
    [[nodiscard]] const std::string& GetLastError() const;
    ReflectedPropertyEdit Draw(std::string_view id,std::string_view label,meta::Variant& value,
        const ReflectedPropertyOptions& options = {});
    template<class T>
    ReflectedPropertyEdit Draw(std::string_view id,std::string_view label,T& value,
        const ReflectedPropertyOptions& options = {})
    {
        meta::Variant reflected(value);
        auto edit = Draw(id,label,reflected,options);
        if (edit.changed) value = reflected.GetValue<T>();
        return edit;
    }

    // Draws a set of same-typed reflected values. Mixed fields are shown with
    // the Inspector's normal mixed-value state; editing a field writes the same
    // value into every target. Include the selection identity in `id` when the
    // caller edits different target sets, so stale per-target values cannot be
    // reused after a selection change.
    ReflectedPropertyEdit DrawValues(std::string_view id,std::string_view label,
        std::span<meta::Variant> values,const ReflectedPropertyOptions& options = {});
    template<class T>
        requires (!std::is_same_v<std::remove_cv_t<T>, meta::Variant>)
    ReflectedPropertyEdit DrawValues(std::string_view id,std::string_view label,
        std::vector<T>& values,const ReflectedPropertyOptions& options = {})
    {
        std::vector<meta::Variant> variants;
        variants.reserve(values.size());
        for (const auto& value : values)
            variants.emplace_back(value);
        auto edit = DrawValues(
            id,
            label,
            std::span<meta::Variant>(variants),
            options);
        if (edit.changed)
        {
            for (size_t index = 0u; index < values.size() && index < variants.size(); ++index)
                values[index] = variants[index].GetValue<T>();
        }
        return edit;
    }

private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
