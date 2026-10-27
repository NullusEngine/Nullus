#include "Properties/PropertyDrawerRegistry.h"
#include "Properties/PropertyControlState.h"
#include "Properties/CurveValue.h"
#include "Properties/ShortcutPropertyDrawer.h"
#include "Properties/CollectionPropertyDrawer.h"
#include "Inspector/PropertyTypeTraits.h"
#include "Serialization/SerializedProperty.h"
#include <UI/Controls/TextControl.h>
#include <UI/Controls/ScalarControl.h>
#include <UI/Controls/VectorControl.h>
#include <Math/ExternalReflection.h>
#include <Math/Matrix4.h>
#include <Math/Bounds.h>
#include <Math/BoundsInt.h>
#include <Math/RectInt.h>
#include <Math/Vector2Int.h>
#include <Math/Vector3Int.h>
#include <Rendering/ExternalReflection.h>
#include <Rendering/Geometry/Bounds.h>
#include <Rendering/Geometry/BoundingSphere.h>
#include <Serialize/SerializedSpecialTypes.h>
#include <LayerMask.h>
#include <Reflection/Enum.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <limits>
#include <mutex>

namespace NLS::Editor::Properties
{
namespace
{
using namespace Inspector;
using namespace UI::Controls;
ControlOptions Options(const PropertyDrawerContext& context)
{
    return {context.property.readOnly || context.property.metadata.readOnly,
            context.serializedProperty && context.serializedProperty->HasMultipleDifferentValues(),
            context.uiScale};
}

template<class T>
bool Publish(PropertyDrawerContext& context, const T& value, ControlResult result)
{
    context.interaction = result;
    if (result.changed && !result.canceled)
        context.changed = context.preview(meta::Variant(value), "Edit " + context.property.displayName);
    return true;
}
template<class T>
T Bound(float number)
{
    if (static_cast<long double>(number) >= (std::numeric_limits<T>::max)())
        return (std::numeric_limits<T>::max)();
    if (static_cast<long double>(number) <= (std::numeric_limits<T>::lowest)())
        return (std::numeric_limits<T>::lowest)();
    return static_cast<T>(number);
}
template<class T>
bool DrawNumber(PropertyDrawerContext& context)
{
    auto value = context.value.GetValue<T>();
    const auto& metadata = context.property.metadata;
    T minimum = (std::numeric_limits<T>::lowest)(), maximum = (std::numeric_limits<T>::max)();
    const auto lower = metadata.rangeMin ? metadata.rangeMin : metadata.min;
    const auto upper = metadata.rangeMax ? metadata.rangeMax : metadata.max;
    if (lower)
        minimum = Bound<T>(*lower);
    if (upper)
        maximum = Bound<T>(*upper);
    std::string error;
    if (metadata.numericMinimum && !EvaluateExpressionAs(*metadata.numericMinimum, minimum, error))
    {
        ImGui::TextDisabled("Invalid minimum: %s", error.c_str());
        return true;
    }
    if (metadata.numericMaximum && !EvaluateExpressionAs(*metadata.numericMaximum, maximum, error))
    {
        ImGui::TextDisabled("Invalid maximum: %s", error.c_str());
        return true;
    }
    if (minimum > maximum)
    {
        ImGui::TextDisabled("Invalid numeric limits");
        return true;
    }
    ScalarOptions scalar;
    scalar.minimum = &minimum;
    scalar.maximum = &maximum;
    scalar.speed = metadata.step.value_or(.01f);
    scalar.drag = std::is_floating_point_v<T>;
    scalar.slider = metadata.rangeMin.has_value() && metadata.rangeMax.has_value();
    return Publish(context, value, Numeric(context.widgetId.c_str(), value, scalar, Options(context)));
}
bool DrawChoice(PropertyDrawerContext& context)
{
    std::vector<SelectionItem> items;
    int selected = -1;
    const auto current = context.value.ToString();
    for (const auto& choice : context.property.choices)
    {
        if ((choice.typedValue.IsValid() ? choice.typedValue.ToString() : choice.value) == current)
            selected = static_cast<int>(items.size());
        items.push_back({choice.label, choice.tooltip, choice.enabled});
    }
    auto& state = *static_cast<PropertyControlState*>(context.state);
    const auto result = Selection(context.widgetId.c_str(), selected, items, state.selection, Options(context));
    auto value = context.value;
    if (result.changed && selected >= 0)
    {
        const auto& choice = context.property.choices[selected];
        value = choice.typedValue.IsValid() ? choice.typedValue : meta::Variant(choice.value);
    }
    return Publish(context, value, result);
}
bool DrawEnum(PropertyDrawerContext& context)
{
    const auto type = context.property.valueType.GetEnum();
    if (!type)
    {
        ImGui::TextDisabled("Unavailable enum");
        return true;
    }
    const auto keys = type.GetKeys();
    std::vector<SelectionItem> items;
    std::vector<uint64_t> masks;
    int selected = -1;
    const auto current = type.GetBits(context.value);
    for (const auto& key : keys)
    {
        auto value = type.GetValue(key);
        if (type.GetBits(value) == current)
            selected = static_cast<int>(items.size());
        items.push_back({FormatInspectorDisplayName(key), {}, true});
        masks.push_back(type.GetBits(value));
    }
    ControlResult result;
    auto value = context.value;
    if (context.property.kind == InspectorPropertyKind::Flags)
    {
        uint64_t bits = static_cast<uint64_t>(current);
        result = Flags(context.widgetId.c_str(), bits, masks, items, Options(context));
        if (result.changed)
        {
            // The canonical stream accepts enum bits independently of whether
            // that exact combination has a named enumerator.
            value = type.FromBits(bits);
        }
    }
    else
    {
        auto& state = *static_cast<PropertyControlState*>(context.state);
        result = Selection(context.widgetId.c_str(), selected, items, state.selection, Options(context));
        if (result.changed && selected >= 0)
            value = type.GetValue(keys[selected]);
    }
    return Publish(context, value, result);
}
bool MixedComplex(PropertyDrawerContext& context)
{
    if (!context.serializedProperty || !context.serializedProperty->HasMultipleDifferentValues())
        return false;
    ImGui::TextDisabled("Mixed");
    ImGui::SameLine();
    if (ImGui::SmallButton("Use primary value"))
        Publish(context, context.value, ControlResult{true, false, true, true, false, ~0ull});
    return true;
}
bool DrawCurve(PropertyDrawerContext& context)
{
    if (MixedComplex(context))
        return true;
    auto value = context.value.GetValue<Engine::Serialize::AnimationCurve>();
    CurveData data;
    data.weights = data.wrap = true;
    data.preWrap = value.preWrapMode;
    data.postWrap = value.postWrapMode;
    for (const auto& key : value.keys)
    {
        CurveKey item;
        item.time = key.time;
        item.value[0] = key.value;
        item.inTangent[0] = key.inTangent;
        item.outTangent[0] = key.outTangent;
        item.inWeight = key.inWeight;
        item.outWeight = key.outWeight;
        item.weightedMode = key.weightedMode;
        data.keys.push_back(item);
    }
    auto& state = *static_cast<PropertyControlState*>(context.state);
    const auto result = Curve(context.widgetId.c_str(), data, state.curve, Options(context));
    if (result.changed)
    {
        value.keys.clear();
        for (const auto& item : data.keys)
        {
            Engine::Serialize::AnimationCurveKeyframe key;
            key.time = static_cast<float>(item.time);
            key.value = item.value[0];
            key.inTangent = item.inTangent[0];
            key.outTangent = item.outTangent[0];
            key.inWeight = item.inWeight;
            key.outWeight = item.outWeight;
            key.weightedMode = item.weightedMode;
            value.keys.push_back(key);
        }
        value.preWrapMode = data.preWrap;
        value.postWrapMode = data.postWrap;
    }
    return Publish(context, value, result);
}
bool DrawGradient(PropertyDrawerContext& context)
{
    if (MixedComplex(context))
        return true;
    auto value = context.value.GetValue<Engine::Serialize::Gradient>();
    GradientData data;
    data.mode = value.mode;
    for (const auto& key : value.colorKeys)
        data.colors.push_back({key.time, {key.color.r, key.color.g, key.color.b, key.color.a}});
    for (const auto& key : value.alphaKeys)
        data.alphas.push_back({key.time, key.alpha});
    auto& state = *static_cast<PropertyControlState*>(context.state);
    const auto result = Gradient(context.widgetId.c_str(), data, state.gradient, Options(context));
    if (result.changed)
    {
        value.colorKeys.clear();
        value.alphaKeys.clear();
        value.mode = data.mode;
        for (const auto& key : data.colors)
            value.colorKeys.push_back({Maths::Color(key.color[0], key.color[1], key.color[2], key.color[3]), key.time});
        for (const auto& key : data.alphas)
            value.alphaKeys.push_back({key.alpha, key.time});
    }
    return Publish(context, value, result);
}
template<class T, size_t N>
bool DrawFloatVector(PropertyDrawerContext& context)
{
    auto value = context.value.GetValue<T>();
    float numbers[N]{};
    if constexpr (N >= 2)
    {
        numbers[0] = value.x;
        numbers[1] = value.y;
    }
    if constexpr (N >= 3)
        numbers[2] = value.z;
    if constexpr (N == 4)
        numbers[3] = value.w;
    const auto result = FloatComponents(context.widgetId.c_str(), numbers, {}, context.property.metadata.step.value_or(.01f), Options(context));
    if (result.changed)
    {
        value.x = numbers[0];
        value.y = numbers[1];
        if constexpr (N >= 3)
            value.z = numbers[2];
        if constexpr (N == 4)
            value.w = numbers[3];
    }
    return Publish(context, value, result);
}
template<class T, size_t N>
bool DrawIntVector(PropertyDrawerContext& context)
{
    auto value = context.value.GetValue<T>();
    int32_t numbers[N]{value.x, value.y};
    if constexpr (N == 3)
        numbers[2] = value.z;
    const auto result = IntComponents(context.widgetId.c_str(), numbers, Options(context));
    value.x = numbers[0];
    value.y = numbers[1];
    if constexpr (N == 3)
        value.z = numbers[2];
    return Publish(context, value, result);
}

bool DrawFloatArrayMatrix(PropertyDrawerContext& context)
{
    auto* property = context.serializedProperty;
    if (!property || !property->IsArray() || property->GetMinArraySize() != 16)
    {
        ImGui::TextDisabled("A 4 x 4 matrix requires 16 values");
        return true;
    }
    std::array<float, 16> values{};
    for (size_t i = 0; i < values.size(); ++i)
        values[i] = property->GetArrayElementAtIndex(i).GetFloatValue();
    ImGui::PushID(context.widgetId.c_str());
    ImGui::BeginGroup();
    if (context.expandedOverride)
        ImGui::SetNextItemOpen(*context.expandedOverride);
    else
        ImGui::SetNextItemOpen(property->IsExpanded(), ImGuiCond_Once);
    const bool open = ImGui::TreeNodeEx("##Matrix", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", context.property.displayName.c_str());
    if (!context.property.metadata.tooltip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", context.property.metadata.tooltip.c_str());
    if (ImGui::BeginPopupContextItem("##MatrixMenu"))
    {
        auto& clipboard = GetCollectionClipboard();
        if (ImGui::MenuItem("Copy"))
        {
            clipboard.value = Serialization::SerializedValue::Capture(property->GetValue());
            clipboard.elements.clear();
        }
        const auto pasted = clipboard.value ? clipboard.value->Resolve(context.property.valueType) : std::nullopt;
        const bool compatible = pasted && pasted->IsArray() && pasted->GetArray().Size() == values.size();
        if (ImGui::MenuItem("Paste", nullptr, false, !context.readOnly && compatible))
        {
            context.changed = context.preview(*pasted, "Paste matrix");
            context.interaction.changed = context.interaction.completed = context.changed;
        }
        if (context.drawMenu)
        {
            ImGui::Separator();
            context.drawMenu();
        }
        ImGui::EndPopup();
    }
    if (open)
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto edited = Matrix("##Values", values, 4, {context.readOnly, property->HasMultipleDifferentValues(), context.uiScale});
        context.interaction.Merge(edited);
        if (context.interaction.changed && !context.interaction.canceled)
        {
            for (size_t i = 0; i < values.size(); ++i)
                if (context.interaction.components & (1ull << i))
                    context.changed |= property->GetArrayElementAtIndex(i).SetFloatValue(values[i]);
            if (context.changed && context.notifyChanged)
                context.notifyChanged(context.property);
        }
        ImGui::TreePop();
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return true;
}
} // namespace
void EnsureBuiltinPropertyDrawers()
{
    static std::once_flag once;
    static std::vector<DrawerRegistrationToken> registrations;
    std::call_once(once, []
                   {
        RegisterShortcutPropertyDrawer();
        const auto add = [](InspectorPropertyKind kind, std::function<bool(PropertyDrawerContext &)> draw) {
            PropertyDrawerRegistration registration;
            registration.scope = DrawerScope::Builtin;
            registration.propertyKind = kind;
            registration.identifier = "builtin.property." + std::to_string(static_cast<int>(kind));
            registration.draw = std::move(draw);
            registrations.push_back(PropertyDrawerRegistry::Get().Register(std::move(registration)));
        };
        add(InspectorPropertyKind::Boolean, [](auto &c) {
            auto v = c.value.template GetValue<bool>();
            return Publish(c, v, Toggle(c.widgetId.c_str(), v, Options(c)));
        });
        add(InspectorPropertyKind::SignedInteger8, DrawNumber<int8_t>);
        add(InspectorPropertyKind::UnsignedInteger8, DrawNumber<uint8_t>);
        add(InspectorPropertyKind::SignedInteger16, DrawNumber<int16_t>);
        add(InspectorPropertyKind::UnsignedInteger16, DrawNumber<uint16_t>);
        add(InspectorPropertyKind::SignedInteger32, DrawNumber<int32_t>);
        add(InspectorPropertyKind::UnsignedInteger32, DrawNumber<uint32_t>);
        add(InspectorPropertyKind::SignedInteger64, DrawNumber<int64_t>);
        add(InspectorPropertyKind::UnsignedInteger64, DrawNumber<uint64_t>);
        add(InspectorPropertyKind::Float, DrawNumber<float>);
        add(InspectorPropertyKind::Double, DrawNumber<double>);
        add(InspectorPropertyKind::String, [](auto &c) {
            auto v = c.value.template GetValue<std::string>();
            auto r = Text(c.widgetId.c_str(), v, c.property.metadata.multiline, Options(c),
                          {0, ImGui::GetTextLineHeight() * 4},
                          c.property.metadata.codeLanguage.empty() ? 0 : ImGuiInputTextFlags_AllowTabInput);
            return Publish(c, v, r);
        });
        add(InspectorPropertyKind::Choice, DrawChoice);
        add(InspectorPropertyKind::Enum, DrawEnum);
        add(InspectorPropertyKind::Flags, DrawEnum);
        add(InspectorPropertyKind::Vector2, DrawFloatVector<Maths::Vector2, 2>);
        add(InspectorPropertyKind::Vector3, DrawFloatVector<Maths::Vector3, 3>);
        add(InspectorPropertyKind::Vector4, DrawFloatVector<Maths::Vector4, 4>);
        add(InspectorPropertyKind::Vector2Int, DrawIntVector<Maths::Vector2Int, 2>);
        add(InspectorPropertyKind::Vector3Int, DrawIntVector<Maths::Vector3Int, 3>);
        add(InspectorPropertyKind::Quaternion, [](auto &c) {
            const auto e = Maths::Quaternion::EulerAngles(c.value.template GetValue<Maths::Quaternion>());
            float v[]{e.x, e.y, e.z};
            const auto r = FloatComponents(c.widgetId.c_str(), v, {}, .01f, Options(c));
            return Publish(c, Maths::Quaternion(Maths::Vector3(v[0], v[1], v[2])), r);
        });
        add(InspectorPropertyKind::Color, [](auto &c) {
            auto color = c.value.template GetValue<Maths::Color>();
            float v[]{color.r, color.g, color.b, color.a};
            const auto r = Color(c.widgetId.c_str(), v, true, Options(c));
            return Publish(c, Maths::Color(v[0], v[1], v[2], v[3]), r);
        });
        add(InspectorPropertyKind::Rect, [](auto &c) {
            auto v = c.value.template GetValue<Maths::Rect>();
            float a[]{v.x, v.y, v.width, v.height};
            const auto r = FloatComponents(c.widgetId.c_str(), a, {}, .01f, Options(c));
            return Publish(c, Maths::Rect(a[0], a[1], a[2], a[3]), r);
        });
        add(InspectorPropertyKind::RectInt, [](auto &c) {
            auto v = c.value.template GetValue<Maths::RectInt>();
            int32_t a[]{v.x, v.y, v.width, v.height};
            const auto r = IntComponents(c.widgetId.c_str(), a, Options(c));
            v.x = a[0];
            v.y = a[1];
            v.width = a[2];
            v.height = a[3];
            return Publish(c, v, r);
        });
        add(InspectorPropertyKind::Matrix, [](auto &c) {
            auto v = c.value.template GetValue<Maths::Matrix4>();
            auto r = Matrix(c.widgetId.c_str(), v.data, 4, Options(c));
            return Publish(c, v, r);
        });
        add(InspectorPropertyKind::Bounds, [](auto &c) {
            ControlResult result;
            if (c.value.GetType() == NLS_TYPEOF(Maths::Bounds))
            {
                auto v = c.value.template GetValue<Maths::Bounds>();
                float center[]{v.center.x, v.center.y, v.center.z}, size[]{v.size.x, v.size.y, v.size.z};
                result.Merge(FloatComponents("Center", center, {}, .01f, Options(c)));
                auto r = FloatComponents("Size", size, {}, .01f, Options(c));
                r.components <<= 3;
                result.Merge(r);
                FinishComposite(result);
                v.center = {center[0], center[1], center[2]};
                v.size = {size[0], size[1], size[2]};
                return Publish(c, v, result);
            }
            auto v = c.value.template GetValue<Render::Geometry::Bounds>();
            float center[]{v.center.x, v.center.y, v.center.z}, size[]{v.size.x, v.size.y, v.size.z};
            result.Merge(FloatComponents("Center", center, {}, .01f, Options(c)));
            auto r = FloatComponents("Size", size, {}, .01f, Options(c));
            r.components <<= 3;
            result.Merge(r);
            FinishComposite(result);
            v.center = {center[0], center[1], center[2]};
            v.size = {size[0], size[1], size[2]};
            return Publish(c, v, result);
        });
        add(InspectorPropertyKind::BoundsInt, [](auto &c) {
            auto v = c.value.template GetValue<Maths::BoundsInt>();
            int32_t center[]{v.position.x, v.position.y, v.position.z}, size[]{v.size.x, v.size.y, v.size.z};
            auto r = IntComponents("Position", center, Options(c));
            auto s = IntComponents("Size", size, Options(c));
            s.components <<= 3;
            r.Merge(s);
            FinishComposite(r);
            v.position = {center[0], center[1], center[2]};
            v.size = {size[0], size[1], size[2]};
            return Publish(c, v, r);
        });
        add(InspectorPropertyKind::BoundingSphere, [](auto &c) {
            auto v = c.value.template GetValue<Render::Geometry::BoundingSphere>();
            float p[]{v.position.x, v.position.y, v.position.z};
            auto r = FloatComponents("Position", p, {}, .01f, Options(c));
            auto s = Numeric("Radius", v.radius);
            s.components <<= 3;
            r.Merge(s);
            FinishComposite(r);
            v.position = {p[0], p[1], p[2]};
            return Publish(c, v, r);
        });
        add(InspectorPropertyKind::LayerMask, [](auto &c) {
            auto v = c.value.template GetValue<Engine::LayerMask>();
            auto mask = v.GetMask();
            const auto r = Numeric(c.widgetId.c_str(), mask, {}, Options(c));
            v.SetMask(mask);
            return Publish(c, v, r);
        });
        add(InspectorPropertyKind::AnimationCurve, DrawCurve);
        add(InspectorPropertyKind::Gradient, DrawGradient);
        PropertyDrawerRegistration curve;
        curve.scope = DrawerScope::ExactType;
        curve.valueType = NLS_TYPEOF(CurveValue);
        curve.identifier = "builtin.multichannel-curve";
        curve.draw = [](auto &c) {
            if (MixedComplex(c))
                return true;
            auto data = ToControlCurve(c.value.template GetValue<CurveValue>());
            const auto result =
                Curve(c.widgetId.c_str(), data, static_cast<PropertyControlState *>(c.state)->curve, Options(c));
            return Publish(c, FromControlCurve(data), result);
        };
        registrations.push_back(PropertyDrawerRegistry::Get().Register(std::move(curve)));
        // The compatibility getter exposes this Matrix4 as Array<float>.
        // Keep that serialized/script contract while editing its fixed cells
        // through the independent matrix control.
        PropertyDrawerRegistration matrix;
        matrix.scope=DrawerScope::Field;
        matrix.ownerTypeName="NLS::Engine::Components::MeshRenderer";
        matrix.fieldPath=PropertyPath("root.userMatrixValues");
        matrix.identifier="builtin.mesh-renderer.user-matrix";
        matrix.draw=DrawFloatArrayMatrix;
        registrations.push_back(PropertyDrawerRegistry::Get().Register(std::move(matrix)));
        add(InspectorPropertyKind::Array, DrawCollectionProperty);
        add(InspectorPropertyKind::Struct, [](auto &c) {
            c.interaction = c.drawStructure ? c.drawStructure() : ControlResult{};
            if (c.serializedProperty)
            {
                c.changed = c.interaction.changed;
                return true;
            }
            return Publish(c, c.value, c.interaction);
        });
        add(InspectorPropertyKind::ObjectReference, [](auto &c) {
            c.interaction = c.drawReference ? c.drawReference() : ControlResult{};
            return Publish(c, c.value, c.interaction);
        }); });
}
} // namespace NLS::Editor::Properties
