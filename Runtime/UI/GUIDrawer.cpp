#include <array>

#include <Utils/PathParser.h>

#include <UI/Widgets/Texts/TextColored.h>
#include <UI/Widgets/Drags/DragSingleScalar.h>
#include <UI/Widgets/Drags/DragMultipleScalars.h>
#include <UI/Widgets/InputFields/InputText.h>
#include <UI/Widgets/Selection/ColorEdit.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Selection/CheckBox.h>
#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Buttons/ButtonSmall.h>
#include <UI/Plugins/DDTarget.h>

#include <ServiceLocator.h>
#include <ResourceManagement/ModelManager.h>
#include <ResourceManagement/TextureManager.h>
#include <ResourceManagement/ShaderManager.h>
#include <ResourceManagement/MaterialManager.h>

#include "GUIDrawer.h"


namespace NLS::UI
{
namespace
{
std::string ToLowerCopy(const std::string &value)
{
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return lower;
}

template <size_t TCount>
void ApplyAxisStyle(
    Widgets::DragMultipleScalars<float, TCount>& widget,
    const std::array<const char*, TCount>& labels,
    const std::array<Maths::Color, TCount>& colors)
{
    widget.axisStyle = true;
    for (size_t i = 0; i < TCount; ++i)
    {
        widget.componentLabels[i] = labels[i];
        widget.componentColors[i] = colors[i];
    }
}

template <size_t TCount>
void ApplyLabeledStyle(Widgets::DragMultipleScalars<float, TCount>& widget, const std::string &fieldName)
{
    static const std::array<Maths::Color, 4> vectorColors = {
        Maths::Color{0.72f, 0.30f, 0.30f, 1.0f},
        Maths::Color{0.31f, 0.62f, 0.35f, 1.0f},
        Maths::Color{0.29f, 0.47f, 0.79f, 1.0f},
        Maths::Color{0.36f, 0.39f, 0.45f, 1.0f}
    };
    static const std::array<Maths::Color, 4> colorColors = {
        Maths::Color{0.72f, 0.30f, 0.30f, 1.0f},
        Maths::Color{0.31f, 0.62f, 0.35f, 1.0f},
        Maths::Color{0.29f, 0.47f, 0.79f, 1.0f},
        Maths::Color{0.52f, 0.52f, 0.56f, 1.0f}
    };

    const std::string loweredName = ToLowerCopy(fieldName);
    const bool isColorField = loweredName.find("color") != std::string::npos;

    if constexpr (TCount == 2)
    {
        ApplyAxisStyle(widget, std::array<const char*, 2>{"X", "Y"}, std::array<Maths::Color, 2>{vectorColors[0], vectorColors[1]});
    }
    else if constexpr (TCount == 3)
    {
        if (isColorField)
            ApplyAxisStyle(widget, std::array<const char*, 3>{"R", "G", "B"}, std::array<Maths::Color, 3>{colorColors[0], colorColors[1], colorColors[2]});
        else
            ApplyAxisStyle(widget, std::array<const char*, 3>{"X", "Y", "Z"}, std::array<Maths::Color, 3>{vectorColors[0], vectorColors[1], vectorColors[2]});
    }
    else if constexpr (TCount == 4)
    {
        if (isColorField)
            ApplyAxisStyle(widget, std::array<const char*, 4>{"R", "G", "B", "A"}, colorColors);
        else
            ApplyAxisStyle(widget, std::array<const char*, 4>{"X", "Y", "Z", "W"}, vectorColors);
    }
}
} // namespace

const Maths::Color GUIDrawer::TitleColor = {0.67f, 0.71f, 0.78f};
const Maths::Color GUIDrawer::ClearButtonColor = {0.36f, 0.17f, 0.17f};
const float GUIDrawer::_MIN_FLOAT = -999999999.f;
const float GUIDrawer::_MAX_FLOAT = +999999999.f;
Render::Resources::Texture2D* GUIDrawer::__EMPTY_TEXTURE = nullptr;

void GUIDrawer::ProvideEmptyTexture(Render::Resources::Texture2D& p_emptyTexture)
{
    __EMPTY_TEXTURE = &p_emptyTexture;
}

void GUIDrawer::CreateTitle(Internal::WidgetContainer& p_root, const std::string& p_name)
{
    p_root.CreateWidget<Widgets::TextColored>(p_name, TitleColor);
}

void GUIDrawer::DrawBoolean(Internal::WidgetContainer& p_root, const std::string& p_name, bool& p_data)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::CheckBox>();
    auto& dispatcher = widget.AddPlugin<DataDispatcher<bool>>();
    dispatcher.RegisterReference(reinterpret_cast<bool&>(p_data));
}

void GUIDrawer::DrawVec2(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector2& p_data, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::DragMultipleScalars<float, 2>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    ApplyLabeledStyle(widget, p_name);
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::array<float, 2>>>();
    dispatcher.RegisterReference(reinterpret_cast<std::array<float, 2>&>(p_data));
}

void GUIDrawer::DrawVec3(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector3& p_data, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::DragMultipleScalars<float, 3>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    ApplyLabeledStyle(widget, p_name);
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::array<float, 3>>>();
    dispatcher.RegisterReference(reinterpret_cast<std::array<float, 3>&>(p_data));
}

void GUIDrawer::DrawVec4(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector4& p_data, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::DragMultipleScalars<float, 4>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    ApplyLabeledStyle(widget, p_name);
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::array<float, 4>>>();
    dispatcher.RegisterReference(reinterpret_cast<std::array<float, 4>&>(p_data));
}

void GUIDrawer::DrawQuat(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Quaternion& p_data, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::DragMultipleScalars<float, 3>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    ApplyLabeledStyle(widget, p_name);
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::array<float, 3>>>();
    dispatcher.RegisterGatherer([&p_data]()
    {
        const Maths::Vector3 euler = Maths::Quaternion::EulerAngles(p_data);
        return std::array<float, 3>{euler.x, euler.y, euler.z};
    });
    dispatcher.RegisterProvider([&p_data](std::array<float, 3> p_value)
    {
        p_data = Maths::Quaternion(Maths::Vector3{p_value[0], p_value[1], p_value[2]});
    });
}

void GUIDrawer::DrawString(Internal::WidgetContainer& p_root, const std::string& p_name, std::string& p_data)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::InputText>("");
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::string>>();
    dispatcher.RegisterReference(p_data);
}

void GUIDrawer::DrawColor(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Color& p_color, bool p_hasAlpha)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::ColorEdit>(p_hasAlpha);
    auto& dispatcher = widget.AddPlugin<DataDispatcher<Maths::Color>>();
    dispatcher.RegisterReference(p_color);
}

Widgets::Text& GUIDrawer::DrawMesh(Internal::WidgetContainer& p_root, const std::string& p_name, Render::Resources::Model*& p_data, Event<>* p_updateNotifier)
{
    CreateTitle(p_root, p_name);

    std::string displayedText = (p_data ? p_data->path : std::string("Empty"));
    auto& rightSide = p_root.CreateWidget<Widgets::Group>();

    auto& widget = rightSide.CreateWidget<Widgets::Text>(displayedText);

    widget.AddPlugin<DDTarget<std::pair<std::string, Widgets::Group*>>>("File").DataReceivedEvent += [&widget, &p_data, p_updateNotifier](auto p_receivedData)
    {
        if (Utils::PathParser::GetFileType(p_receivedData.first) == Utils::PathParser::EFileType::MODEL)
        {
            if (auto resource = NLS_SERVICE(Core::ResourceManagement::ModelManager).GetResource(p_receivedData.first); resource)
            {
                p_data = resource;
                widget.content = p_receivedData.first;
                if (p_updateNotifier)
                    p_updateNotifier->Invoke();
            }
        }
    };

    widget.lineBreak = false;

    auto& resetButton = rightSide.CreateWidget<Widgets::ButtonSmall>("Clear");
    resetButton.idleBackgroundColor = ClearButtonColor;
    resetButton.ClickedEvent += [&widget, &p_data, p_updateNotifier]
    {
        p_data = nullptr;
        widget.content = "Empty";
        if (p_updateNotifier)
            p_updateNotifier->Invoke();
    };

    return widget;
}

Widgets::Image& GUIDrawer::DrawTexture(Internal::WidgetContainer& p_root, const std::string& p_name, Render::Resources::Texture2D*& p_data, Event<>* p_updateNotifier)
{
    CreateTitle(p_root, p_name);

    std::string displayedText = (p_data ? p_data->path : std::string("Empty"));
    auto& rightSide = p_root.CreateWidget<Widgets::Group>();

    auto& widget = rightSide.CreateWidget<Widgets::Image>(p_data ? p_data->GetTextureId() : (__EMPTY_TEXTURE ? __EMPTY_TEXTURE->GetTextureId() : 0), Maths::Vector2{75, 75});

    widget.AddPlugin<DDTarget<std::pair<std::string, Widgets::Group*>>>("File").DataReceivedEvent += [&widget, &p_data, p_updateNotifier](auto p_receivedData)
    {
        if (Utils::PathParser::GetFileType(p_receivedData.first) == Utils::PathParser::EFileType::TEXTURE)
        {
            if (auto resource = NLS_SERVICE(Core::ResourceManagement::TextureManager).GetResource(p_receivedData.first); resource)
            {
                p_data = static_cast<Render::Resources::Texture2D*>(resource);
                widget.textureID.id = resource->GetTextureId();
                if (p_updateNotifier)
                    p_updateNotifier->Invoke();
            }
        }
    };

    widget.lineBreak = false;

    auto& resetButton = rightSide.CreateWidget<Widgets::Button>("Clear");
    resetButton.idleBackgroundColor = ClearButtonColor;
    resetButton.ClickedEvent += [&widget, &p_data, p_updateNotifier]
    {
        p_data = nullptr;
        widget.textureID.id = (__EMPTY_TEXTURE ? __EMPTY_TEXTURE->GetTextureId() : 0);
        if (p_updateNotifier)
            p_updateNotifier->Invoke();
    };

    return widget;
}

Widgets::Text& GUIDrawer::DrawShader(Internal::WidgetContainer& p_root, const std::string& p_name, Render::Resources::Shader*& p_data, Event<>* p_updateNotifier)
{
    CreateTitle(p_root, p_name);

    std::string displayedText = (p_data ? p_data->path : std::string("Empty"));
    auto& rightSide = p_root.CreateWidget<Widgets::Group>();

    auto& widget = rightSide.CreateWidget<Widgets::Text>(displayedText);

    widget.AddPlugin<DDTarget<std::pair<std::string, Widgets::Group*>>>("File").DataReceivedEvent += [&widget, &p_data, p_updateNotifier](auto p_receivedData)
    {
        if (Utils::PathParser::GetFileType(p_receivedData.first) == Utils::PathParser::EFileType::SHADER)
        {
            if (auto resource = NLS_SERVICE(Core::ResourceManagement::ShaderManager).GetResource(p_receivedData.first); resource)
            {
                p_data = resource;
                widget.content = p_receivedData.first;
                if (p_updateNotifier)
                    p_updateNotifier->Invoke();
            }
        }
    };

    widget.lineBreak = false;

    auto& resetButton = rightSide.CreateWidget<Widgets::ButtonSmall>("Clear");
    resetButton.idleBackgroundColor = ClearButtonColor;
    resetButton.ClickedEvent += [&widget, &p_data, p_updateNotifier]
    {
        p_data = nullptr;
        widget.content = "Empty";
        if (p_updateNotifier)
            p_updateNotifier->Invoke();
    };

    return widget;
}

Widgets::Text& GUIDrawer::DrawMaterial(Internal::WidgetContainer& p_root, const std::string& p_name, NLS::Render::Resources::Material*& p_data, Event<>* p_updateNotifier)
{
    CreateTitle(p_root, p_name);

    std::string displayedText = (p_data ? p_data->path : std::string("Empty"));
    auto& rightSide = p_root.CreateWidget<Widgets::Group>();

    auto& widget = rightSide.CreateWidget<Widgets::Text>(displayedText);

    widget.AddPlugin<DDTarget<std::pair<std::string, Widgets::Group*>>>("File").DataReceivedEvent += [&widget, &p_data, p_updateNotifier](auto p_receivedData)
    {
        if (Utils::PathParser::GetFileType(p_receivedData.first) == Utils::PathParser::EFileType::MATERIAL)
        {
            if (auto resource = NLS_SERVICE(Core::ResourceManagement::MaterialManager).GetResource(p_receivedData.first); resource)
            {
                p_data = resource;
                widget.content = p_receivedData.first;
                if (p_updateNotifier)
                    p_updateNotifier->Invoke();
            }
        }
    };

    widget.lineBreak = false;

    auto& resetButton = rightSide.CreateWidget<Widgets::ButtonSmall>("Clear");
    resetButton.idleBackgroundColor = ClearButtonColor;
    resetButton.ClickedEvent += [&widget, &p_data, p_updateNotifier]
    {
        p_data = nullptr;
        widget.content = "Empty";
        if (p_updateNotifier)
            p_updateNotifier->Invoke();
    };

    return widget;
}


Widgets::Text& GUIDrawer::DrawAsset(Internal::WidgetContainer& p_root, const std::string& p_name, std::string& p_data, Event<>* p_updateNotifier)
{
    CreateTitle(p_root, p_name);

    const std::string displayedText = (p_data.empty() ? std::string("Empty") : p_data);
    auto& rightSide = p_root.CreateWidget<Widgets::Group>();

    auto& widget = rightSide.CreateWidget<Widgets::Text>(displayedText);

    widget.AddPlugin<DDTarget<std::pair<std::string, Widgets::Group*>>>("File").DataReceivedEvent += [&widget, &p_data, p_updateNotifier](auto p_receivedData)
    {
        p_data = p_receivedData.first;
        widget.content = p_receivedData.first;
        if (p_updateNotifier)
            p_updateNotifier->Invoke();
    };

    widget.lineBreak = false;

    auto& resetButton = rightSide.CreateWidget<Widgets::ButtonSmall>("Clear");
    resetButton.idleBackgroundColor = ClearButtonColor;
    resetButton.ClickedEvent += [&widget, &p_data, p_updateNotifier]
    {
        p_data = "";
        widget.content = "Empty";
        if (p_updateNotifier)
            p_updateNotifier->Invoke();
    };

    return widget;
}

void GUIDrawer::DrawBoolean(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<bool(void)> p_gatherer, std::function<void(bool)> p_provider)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::CheckBox>();
    auto& dispatcher = widget.AddPlugin<DataDispatcher<bool>>();

    dispatcher.RegisterGatherer([p_gatherer]()
                                {
				bool value = p_gatherer();
				return reinterpret_cast<bool&>(value); });

    dispatcher.RegisterProvider([p_provider](bool p_value)
                                { p_provider(reinterpret_cast<bool&>(p_value)); });
}

void GUIDrawer::DrawVec2(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Vector2(void)> p_gatherer, std::function<void(Maths::Vector2)> p_provider, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::DragMultipleScalars<float, 2>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    ApplyLabeledStyle(widget, p_name);
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::array<float, 2>>>();

    dispatcher.RegisterGatherer([p_gatherer]()
                                {
				Maths::Vector2 value = p_gatherer();
				return reinterpret_cast<const std::array<float, 2>&>(value); });

    dispatcher.RegisterProvider([p_provider](std::array<float, 2> p_value)
                                { p_provider(reinterpret_cast<Maths::Vector2&>(p_value)); });
}

void GUIDrawer::DrawVec3(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Vector3(void)> p_gatherer, std::function<void(Maths::Vector3)> p_provider, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::DragMultipleScalars<float, 3>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    ApplyLabeledStyle(widget, p_name);
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::array<float, 3>>>();

    dispatcher.RegisterGatherer([p_gatherer]()
                                {
				Maths::Vector3 value = p_gatherer();
				return reinterpret_cast<const std::array<float, 3>&>(value); });

    dispatcher.RegisterProvider([p_provider](std::array<float, 3> p_value)
                                { p_provider(reinterpret_cast<Maths::Vector3&>(p_value)); });
}

void GUIDrawer::DrawVec4(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Vector4(void)> p_gatherer, std::function<void(Maths::Vector4)> p_provider, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::DragMultipleScalars<float, 4>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    ApplyLabeledStyle(widget, p_name);
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::array<float, 4>>>();

    dispatcher.RegisterGatherer([p_gatherer]()
                                {
				Maths::Vector4 value = p_gatherer();
				return reinterpret_cast<const std::array<float, 4>&>(value); });

    dispatcher.RegisterProvider([p_provider](std::array<float, 4> p_value)
                                { p_provider(reinterpret_cast<Maths::Vector4&>(p_value)); });
}

void GUIDrawer::DrawQuat(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Quaternion(void)> p_gatherer, std::function<void(Maths::Quaternion)> p_provider, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::DragMultipleScalars<float, 3>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    ApplyLabeledStyle(widget, p_name);
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::array<float, 3>>>();

    dispatcher.RegisterGatherer([p_gatherer]()
                                {
				const Maths::Vector3 euler = Maths::Quaternion::EulerAngles(p_gatherer());
				return std::array<float, 3>{euler.x, euler.y, euler.z}; });

    dispatcher.RegisterProvider([p_provider](std::array<float, 3> p_value)
                                { p_provider(Maths::Quaternion(Maths::Vector3{p_value[0], p_value[1], p_value[2]})); });
}

void GUIDrawer::DrawDDString(Internal::WidgetContainer& p_root, const std::string& p_name,
                             std::function<std::string()> p_gatherer, std::function<void(std::string)> p_provider,
                             const std::string& p_identifier)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::InputText>("");
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::string>>();
    dispatcher.RegisterGatherer(p_gatherer);
    dispatcher.RegisterProvider(p_provider);

    auto& ddTarget = widget.AddPlugin<DDTarget<std::pair<std::string, Widgets::Group*>>>(p_identifier);
    ddTarget.DataReceivedEvent += [&widget, &dispatcher](std::pair<std::string, Widgets::Group*> p_data)
    {
        widget.content = p_data.first;
        dispatcher.NotifyChange();
    };
}

void GUIDrawer::DrawString(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<std::string(void)> p_gatherer, std::function<void(std::string)> p_provider)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::InputText>("");
    auto& dispatcher = widget.AddPlugin<DataDispatcher<std::string>>();
    dispatcher.RegisterGatherer(p_gatherer);
    dispatcher.RegisterProvider(p_provider);
}

void GUIDrawer::DrawColor(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Color(void)> p_gatherer, std::function<void(Maths::Color)> p_provider, bool p_hasAlpha)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::ColorEdit>(p_hasAlpha);
    auto& dispatcher = widget.AddPlugin<DataDispatcher<Maths::Color>>();
    dispatcher.RegisterGatherer(p_gatherer);
    dispatcher.RegisterProvider(p_provider);
}

} // namespace NLS::UI
