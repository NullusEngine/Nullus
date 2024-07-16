#pragma once

#include <Math/Vector2.h>
#include <Math/Vector3.h>
#include <Math/Vector4.h>
#include <Math/Quaternion.h>

#include <UI/Internal/WidgetContainer.h>
#include <UI/Widgets/Texts/Text.h>
#include <UI/Widgets/Drags/DragSingleScalar.h>
#include <UI/Widgets/Drags/DragMultipleScalars.h>
#include <UI/Widgets/InputFields/InputText.h>
#include <UI/Widgets/Visual/Image.h>
#include <Math/Color.h>
#include "Resources/Material.h"
#include <limits>
#include <type_traits>

namespace NLS::Render::Resources
{
class Material;
class Model;
class Shader;
class Texture;
} // namespace NLS::Render::Resources

namespace NLS::UI
{
/**
 * Provide some helpers to draw UI elements
 */
class NLS_CORE_API GUIDrawer
{
public:
    static const Maths::Color TitleColor;
    static const Maths::Color ClearButtonColor;

    static const float _MIN_FLOAT;
    static const float _MAX_FLOAT;

    /**
     * Defines the texture to use when there is no texture in a texture resource field
     * @param p_emptyTexture
     */
    static void ProvideEmptyTexture(Render::Resources::Texture& p_emptyTexture);

    /**
     * Draw a title with the title color
     * @param p_root
     * @param p_name
     */
    static void CreateTitle(UI::Internal::WidgetContainer& p_root, const std::string& p_name);

    template<typename T>
    static void DrawScalar(Internal::WidgetContainer& p_root, const std::string& p_name, T& p_data, float p_step = 1.f, T p_min = std::numeric_limits<T>::min(), T p_max = std::numeric_limits<T>::max())
    {
        static_assert(std::is_scalar<T>::value, "T must be a scalar");

        CreateTitle(p_root, p_name);
        auto& widget = p_root.CreateWidget<Widgets::Drags::DragSingleScalar<T>>(GetDataType<T>(), p_min, p_max, p_data, p_step, "", GetFormat<T>());
        auto& dispatcher = widget.template AddPlugin<Plugins::DataDispatcher<T>>();
        dispatcher.RegisterReference(p_data);
    }
    static void DrawBoolean(Internal::WidgetContainer& p_root, const std::string& p_name, bool& p_data);
    static void DrawVec2(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector2& p_data, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
    static void DrawVec3(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector3& p_data, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
    static void DrawVec4(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector4& p_data, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
    static void DrawQuat(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Quaternion& p_data, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
    static void DrawString(Internal::WidgetContainer& p_root, const std::string& p_name, std::string& p_data);
    static void DrawColor(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Color& p_color, bool p_hasAlpha = false);
    static Widgets::Texts::Text& DrawMesh(Internal::WidgetContainer& p_root, const std::string& p_name, Render::Resources::Model*& p_data, Event<>* p_updateNotifier = nullptr);
    static Widgets::Visual::Image& DrawTexture(Internal::WidgetContainer& p_root, const std::string& p_name, Render::Resources::Texture*& p_data, Event<>* p_updateNotifier = nullptr);
    static Widgets::Texts::Text& DrawShader(Internal::WidgetContainer& p_root, const std::string& p_name, Render::Resources::Shader*& p_data, Event<>* p_updateNotifier = nullptr);
    static Widgets::Texts::Text& DrawMaterial(Internal::WidgetContainer& p_root, const std::string& p_name, NLS::Render::Resources::Material*& p_data, Event<>* p_updateNotifier = nullptr);
    // static Widgets::Texts::Text& DrawSound(Internal::WidgetContainer& p_root, const std::string& p_name, OvAudio::Resources::Sound*& p_data, Event<>* p_updateNotifier = nullptr);
    static Widgets::Texts::Text& DrawAsset(Internal::WidgetContainer& p_root, const std::string& p_name, std::string& p_data, Event<>* p_updateNotifier = nullptr);

    template<typename T>
    static void DrawScalar(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<T(void)> p_gatherer, std::function<void(T)> p_provider, float p_step = 1.f, T p_min = std::numeric_limits<T>::min(), T p_max = std::numeric_limits<T>::max())
    {
        static_assert(std::is_scalar<T>::value, "T must be a scalar");

        CreateTitle(p_root, p_name);
        auto& widget = p_root.CreateWidget<Widgets::Drags::DragSingleScalar<T>>(GetDataType<T>(), p_min, p_max, static_cast<T>(0), p_step, "", GetFormat<T>());
        auto& dispatcher = widget.template AddPlugin<Plugins::DataDispatcher<T>>();
        dispatcher.RegisterGatherer(p_gatherer);
        dispatcher.RegisterProvider(p_provider);
    }
    static void DrawBoolean(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<bool(void)> p_gatherer, std::function<void(bool)> p_provider);
    static void DrawVec2(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Vector2(void)> p_gatherer, std::function<void(Maths::Vector2)> p_provider, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
    static void DrawVec3(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Vector3(void)> p_gatherer, std::function<void(Maths::Vector3)> p_provider, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
    static void DrawVec4(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Vector4(void)> p_gatherer, std::function<void(Maths::Vector4)> p_provider, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
    static void DrawQuat(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Quaternion(void)> p_gatherer, std::function<void(Maths::Quaternion)> p_provider, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
    static void DrawDDString(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<std::string(void)> p_gatherer, std::function<void(std::string)> p_provider, const std::string& p_identifier);
    static void DrawString(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<std::string(void)> p_gatherer, std::function<void(std::string)> p_provider);
    static void DrawColor(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Color(void)> p_gatherer, std::function<void(Maths::Color)> p_provider, bool p_hasAlpha = false);

    template<typename T>
    static ImGuiDataType_ GetDataType()
    {
        if constexpr (std::is_same<T, float>::value)
            return ImGuiDataType_Float;
        else if constexpr (std::is_same<T, double>::value)
            return ImGuiDataType_Double;
        else if constexpr (std::is_same<T, uint8_t>::value)
            return ImGuiDataType_U8;
        else if constexpr (std::is_same<T, uint16_t>::value)
            return ImGuiDataType_U16;
        else if constexpr (std::is_same<T, uint32_t>::value)
            return ImGuiDataType_U32;
        else if constexpr (std::is_same<T, uint64_t>::value)
            return ImGuiDataType_U64;
        else if constexpr (std::is_same<T, int8_t>::value)
            return ImGuiDataType_S8;
        else if constexpr (std::is_same<T, int16_t>::value)
            return ImGuiDataType_S16;
        else if constexpr (std::is_same<T, int32_t>::value)
            return ImGuiDataType_S32;
        else if constexpr (std::is_same<T, int64_t>::value)
            return ImGuiDataType_S64;
    }

    template<typename T>
    static std::string GetFormat()
    {
        if constexpr (std::is_same<T, double>::value)
            return "%.5f";
        else if constexpr (std::is_same<T, float>::value)
            return "%.3f";
        else
            return "%d";
    }

private:
    static Render::Resources::Texture* __EMPTY_TEXTURE;
};
} // namespace NLS::UI
