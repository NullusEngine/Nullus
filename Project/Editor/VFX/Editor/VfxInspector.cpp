#include "VfxInspector.h"
#include <imgui.h>
#include <type_traits>
#include <mutex>
#include "Inspector/EditorReflection.h"
#include "Inspector/PropertyTypeTraits.h"
#include "Inspector/ReflectedEditHost.h"

namespace NLS::Editor::VFXTools
{
namespace
{
    // Bridges ReflectedPropertyEditor edits to VfxDocument's working/preview
    // model. The shared ApplyReflectedEdit owns the changed/completed/canceled
    // state machine.
    class VfxPropertyEditHost final : public Inspector::IReflectedEditHost
    {
    public:
        VfxPropertyEditHost(VfxDocument& document, const VFX::Authoring::System& candidate)
            : m_document(document), m_candidate(candidate)
        {
        }

        [[nodiscard]] bool BeginPropertyEdit(const std::string_view label) override
        {
            return m_document.BeginEdit(std::string(label));
        }

        [[nodiscard]] bool PreviewPropertyEdit() override
        {
            if (m_document.PreviewEdit(m_candidate))
            {
                m_error.clear();
                return true;
            }
            m_error = m_document.Error();
            return false;
        }

        [[nodiscard]] bool CommitPropertyEdit() override
        {
            if (m_document.EndEdit())
            {
                m_error.clear();
                return true;
            }
            m_error = m_document.Error();
            return false;
        }

        [[nodiscard]] bool CancelPropertyEdit() override
        {
            (void)m_document.EndEdit(false);
            return true;
        }

        [[nodiscard]] bool IsPropertyEditOpen() const override
        {
            return m_document.IsEditing();
        }

        [[nodiscard]] std::string_view GetPropertyEditError() const override
        {
            return m_error;
        }

    private:
        VfxDocument& m_document;
        const VFX::Authoring::System& m_candidate;
        std::string m_error;
    };
}

inline void RegisterVfxInspectorReflection(meta::ReflectionDatabase& db,meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::VFX::SimulationTarget)
        metaExternalType.SetEnum<NLS::VFX::SimulationTarget>(metaExternalTypeName,
            {{"CPU",VFX::SimulationTarget::CPU},{"GPU Compute Sim",VFX::SimulationTarget::GPU}});
    NLS_META_EXTERNAL_END();
    NLS_META_EXTERNAL_BEGIN(NLS::VFX::SpriteBlendMode)
        metaExternalType.SetEnum<NLS::VFX::SpriteBlendMode>(metaExternalTypeName,
            {{"Translucent",VFX::SpriteBlendMode::Translucent},{"Additive",VFX::SpriteBlendMode::Additive},
             {"Masked",VFX::SpriteBlendMode::Masked},{"Opaque",VFX::SpriteBlendMode::Opaque},
             {"Premultiplied",VFX::SpriteBlendMode::Premultiplied}});
    NLS_META_EXTERNAL_END();
    NLS_META_EXTERNAL_BEGIN(NLS::VFX::Authoring::InputMode)
        metaExternalType.SetEnum<NLS::VFX::Authoring::InputMode>(metaExternalTypeName,
            {{"Local Value",VFX::Authoring::InputMode::Literal},{"Linked Input",VFX::Authoring::InputMode::Parameter},
             {"Dynamic Input",VFX::Authoring::InputMode::DynamicInput},{"Curve / Gradient",VFX::Authoring::InputMode::Curve},
             {"Script Asset",VFX::Authoring::InputMode::Script}});
    NLS_META_EXTERNAL_END();
    NLS_META_EXTERNAL_BEGIN(NLS::VFX::ValueType)
        metaExternalType.SetEnum<NLS::VFX::ValueType>(metaExternalTypeName,
            {{"Float",VFX::ValueType::Float},{"Vector2",VFX::ValueType::Float2},{"Vector3",VFX::ValueType::Float3},
             {"Vector4 / Color",VFX::ValueType::Float4},{"Int32",VFX::ValueType::Int32},{"Bool",VFX::ValueType::Bool}});
    NLS_META_EXTERNAL_END();
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::VFXTools::VfxSystemDetails)
        NLS_META_EXTERNAL_FIELD(bool,fixedTick);
        NLS_META_EXTERNAL_FIELD(double,fixedTickDelta);
        NLS_META_EXTERNAL_FIELD(double,warmupTime);
        NLS_META_EXTERNAL_FIELD(uint32_t,warmupTickCount);
        NLS_META_EXTERNAL_FIELD(double,warmupTickDelta);
    NLS_META_EXTERNAL_END();
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::VFXTools::VfxEmitterDetails)
        NLS_META_EXTERNAL_FIELD(std::string,name);
        NLS_META_EXTERNAL_FIELD(bool,enabled);
        NLS_META_EXTERNAL_FIELD(NLS::VFX::SimulationTarget,target);
        NLS_META_EXTERNAL_FIELD(uint32_t,capacity);
        NLS_META_EXTERNAL_FIELD(bool,sortByViewDepth);
        NLS_META_EXTERNAL_FIELD(bool,frustumCulling);
    NLS_META_EXTERNAL_END();
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::VFXTools::VfxStageDetails)
        NLS_META_EXTERNAL_FIELD(bool,enabled);
        NLS_META_EXTERNAL_FIELD(std::string,name);
        NLS_META_EXTERNAL_FIELD(uint32_t,iterations);
    NLS_META_EXTERNAL_END();
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::VFXTools::VfxRendererDetails)
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Color,color);
        NLS_META_EXTERNAL_FIELD(NLS::VFX::SpriteBlendMode,blend);
    NLS_META_EXTERNAL_END();
}
NLS_META_EXTERNAL_MODULE(RegisterVfxInspectorReflection)

void EnsureVfxInspectorReflection()
{
    (void)meta::ReflectionDatabase::Instance();
    static std::once_flag once;
    std::call_once(once,[]
    {
        Inspector::RegisterEditorStructMetadata(NLS_TYPEOF(VfxEmitterDetails), {
            { "target", "Sim Target" },
            { "capacity", "Max Particles" }
        });
        Inspector::RegisterEditorStructMetadata(NLS_TYPEOF(VfxSystemDetails), {
            { "fixedTick", "Fixed Tick Delta Time" }
        });
        Inspector::RegisterEditorStructMetadata(NLS_TYPEOF(VfxStageDetails), {
            { "name", "Simulation Stage Name" },
            { "iterations", "Num Iterations" }
        });
    });
}

bool CommitInspectorEdit(VfxDocument& document,const VFX::Authoring::System& candidate,
    const Inspector::ReflectedPropertyEdit& edit,const char* label)
{
    VfxPropertyEditHost host(document, candidate);
    return Inspector::ApplyReflectedEdit(host, edit, label);
}

Inspector::ReflectedPropertyEdit DrawVfxValue(Inspector::ReflectedPropertyEditor& inspector,
                                              const char* label, VFX::ValueType type, VFX::Value& value, const Inspector::ReflectedPropertyOptions& options)
{
    using namespace VFX;
    const auto id = std::to_string(ImGui::GetID(label));
    const auto draw = [&]<class T>(T native)
    {
        auto edit = inspector.Draw(id, label, native, options);
        if (edit.changed)
        {
            if constexpr (std::is_same_v<T, bool>)
                value = BoolValue(native);
            else if constexpr (std::is_same_v<T, int32_t>)
                value = IntValue(native);
            else if constexpr (std::is_same_v<T, float>)
                value[0] = native;
            else if constexpr (std::is_same_v<T, Maths::Vector2>)
            {
                value[0] = native.x;
                value[1] = native.y;
            }
            else if constexpr (std::is_same_v<T, Maths::Vector3>)
            {
                value[0] = native.x;
                value[1] = native.y;
                value[2] = native.z;
            }
            else if constexpr (std::is_same_v<T, Maths::Color>)
                value = {native.r, native.g, native.b, native.a};
            else
                value = {native.x, native.y, native.z, native.w};
        }
        return edit;
    };
    switch (type)
    {
    case ValueType::Bool: return draw(ReadBool(value));
    case ValueType::Int32: return draw(ReadInt(value));
    case ValueType::Float: return draw(value[0]);
    case ValueType::Float2: return draw(Maths::Vector2(value[0],value[1]));
    case ValueType::Float3: return draw(Maths::Vector3(value[0],value[1],value[2]));
    case ValueType::Float4:
        if (options.metadata && options.metadata->colorUsage)
            return draw(Maths::Color(value[0], value[1], value[2], value[3]));
        return draw(Maths::Vector4(value[0], value[1], value[2], value[3]));
    }
    return {};
}

Inspector::ReflectedPropertyEdit DrawVfxValueType(Inspector::ReflectedPropertyEditor& inspector,
                                                  const char* label, VFX::ValueType& type, VFX::Value& value)
{
    EnsureVfxInspectorReflection();
    const auto previous = type;
    Inspector::ReflectedPropertyOptions options;
    options.layout = Inspector::PropertyEditorLayout::ValueOnly;
    const auto edit = inspector.Draw(std::to_string(ImGui::GetID(label)), label, type, options);
    if (edit.changed && (VFX::IsIntegralType(type) || VFX::IsIntegralType(previous)))
        value = {};
    return edit;
}
} // namespace NLS::Editor::VFXTools
