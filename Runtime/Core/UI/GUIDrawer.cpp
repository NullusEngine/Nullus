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
const Maths::Color GUIDrawer::TitleColor = {0.85f, 0.65f, 0.0f};
const Maths::Color GUIDrawer::ClearButtonColor = {0.5f, 0.0f, 0.0f};
const float GUIDrawer::_MIN_FLOAT = -999999999.f;
const float GUIDrawer::_MAX_FLOAT = +999999999.f;
Render::Resources::Texture2D* GUIDrawer::__EMPTY_TEXTURE = nullptr;

void GUIDrawer::ProvideEmptyTexture(Render::Resources::Texture2D& p_emptyTexture)
{
    __EMPTY_TEXTURE = &p_emptyTexture;
}

void GUIDrawer::CreateTitle(Internal::WidgetContainer& p_root, const std::string& p_name)
{
    p_root.CreateWidget<Widgets::Texts::TextColored>(p_name, TitleColor);
}

void GUIDrawer::DrawBoolean(Internal::WidgetContainer& p_root, const std::string& p_name, bool& p_data)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::Selection::CheckBox>();
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<bool>>();
    dispatcher.RegisterReference(reinterpret_cast<bool&>(p_data));
}

void GUIDrawer::DrawVec2(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector2& p_data, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::Drags::DragMultipleScalars<float, 2>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::array<float, 2>>>();
    dispatcher.RegisterReference(reinterpret_cast<std::array<float, 2>&>(p_data));
}

void GUIDrawer::DrawVec3(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector3& p_data, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::Drags::DragMultipleScalars<float, 3>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::array<float, 3>>>();
    dispatcher.RegisterReference(reinterpret_cast<std::array<float, 3>&>(p_data));
}

void GUIDrawer::DrawVec4(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector4& p_data, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::Drags::DragMultipleScalars<float, 4>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::array<float, 4>>>();
    dispatcher.RegisterReference(reinterpret_cast<std::array<float, 4>&>(p_data));
}

void GUIDrawer::DrawQuat(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Quaternion& p_data, float p_step, float p_min, float p_max)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::Drags::DragMultipleScalars<float, 4>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::array<float, 4>>>();
    dispatcher.RegisterReference(reinterpret_cast<std::array<float, 4>&>(p_data));
}

void GUIDrawer::DrawString(Internal::WidgetContainer& p_root, const std::string& p_name, std::string& p_data)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::InputFields::InputText>("");
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::string>>();
    dispatcher.RegisterReference(p_data);
}

void GUIDrawer::DrawColor(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Color& p_color, bool p_hasAlpha)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::Selection::ColorEdit>(p_hasAlpha);
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<Maths::Color>>();
    dispatcher.RegisterReference(p_color);
}

Widgets::Texts::Text& GUIDrawer::DrawMesh(Internal::WidgetContainer& p_root, const std::string& p_name, Render::Resources::Model*& p_data, Event<>* p_updateNotifier)
{
    CreateTitle(p_root, p_name);

    std::string displayedText = (p_data ? p_data->path : std::string("Empty"));
    auto& rightSide = p_root.CreateWidget<Widgets::Layout::Group>();

    auto& widget = rightSide.CreateWidget<Widgets::Texts::Text>(displayedText);

    widget.AddPlugin<Plugins::DDTarget<std::pair<std::string, Widgets::Layout::Group*>>>("File").DataReceivedEvent += [&widget, &p_data, p_updateNotifier](auto p_receivedData)
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

    auto& resetButton = rightSide.CreateWidget<Widgets::Buttons::ButtonSmall>("Clear");
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

Widgets::Visual::Image& GUIDrawer::DrawTexture(Internal::WidgetContainer& p_root, const std::string& p_name, Render::Resources::Texture2D*& p_data, Event<>* p_updateNotifier)
{
    CreateTitle(p_root, p_name);

    std::string displayedText = (p_data ? p_data->path : std::string("Empty"));
    auto& rightSide = p_root.CreateWidget<Widgets::Layout::Group>();

    auto& widget = rightSide.CreateWidget<Widgets::Visual::Image>(p_data ? p_data->GetTextureId() : (__EMPTY_TEXTURE ? __EMPTY_TEXTURE->GetTextureId() : 0), Maths::Vector2{75, 75});

    widget.AddPlugin<Plugins::DDTarget<std::pair<std::string, Widgets::Layout::Group*>>>("File").DataReceivedEvent += [&widget, &p_data, p_updateNotifier](auto p_receivedData)
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

    auto& resetButton = rightSide.CreateWidget<Widgets::Buttons::Button>("Clear");
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

Widgets::Texts::Text& GUIDrawer::DrawShader(Internal::WidgetContainer& p_root, const std::string& p_name, Render::Resources::Shader*& p_data, Event<>* p_updateNotifier)
{
    CreateTitle(p_root, p_name);

    std::string displayedText = (p_data ? p_data->path : std::string("Empty"));
    auto& rightSide = p_root.CreateWidget<Widgets::Layout::Group>();

    auto& widget = rightSide.CreateWidget<Widgets::Texts::Text>(displayedText);

    widget.AddPlugin<Plugins::DDTarget<std::pair<std::string, Widgets::Layout::Group*>>>("File").DataReceivedEvent += [&widget, &p_data, p_updateNotifier](auto p_receivedData)
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

    auto& resetButton = rightSide.CreateWidget<Widgets::Buttons::ButtonSmall>("Clear");
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

Widgets::Texts::Text& GUIDrawer::DrawMaterial(Internal::WidgetContainer& p_root, const std::string& p_name, NLS::Render::Resources::Material*& p_data, Event<>* p_updateNotifier)
{
    CreateTitle(p_root, p_name);

    std::string displayedText = (p_data ? p_data->path : std::string("Empty"));
    auto& rightSide = p_root.CreateWidget<Widgets::Layout::Group>();

    auto& widget = rightSide.CreateWidget<Widgets::Texts::Text>(displayedText);

    widget.AddPlugin<Plugins::DDTarget<std::pair<std::string, Widgets::Layout::Group*>>>("File").DataReceivedEvent += [&widget, &p_data, p_updateNotifier](auto p_receivedData)
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

    auto& resetButton = rightSide.CreateWidget<Widgets::Buttons::ButtonSmall>("Clear");
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


Widgets::Texts::Text& GUIDrawer::DrawAsset(Internal::WidgetContainer& p_root, const std::string& p_name, std::string& p_data, Event<>* p_updateNotifier)
{
    CreateTitle(p_root, p_name);

    const std::string displayedText = (p_data.empty() ? std::string("Empty") : p_data);
    auto& rightSide = p_root.CreateWidget<Widgets::Layout::Group>();

    auto& widget = rightSide.CreateWidget<Widgets::Texts::Text>(displayedText);

    widget.AddPlugin<Plugins::DDTarget<std::pair<std::string, Widgets::Layout::Group*>>>("File").DataReceivedEvent += [&widget, &p_data, p_updateNotifier](auto p_receivedData)
    {
        p_data = p_receivedData.first;
        widget.content = p_receivedData.first;
        if (p_updateNotifier)
            p_updateNotifier->Invoke();
    };

    widget.lineBreak = false;

    auto& resetButton = rightSide.CreateWidget<Widgets::Buttons::ButtonSmall>("Clear");
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
    auto& widget = p_root.CreateWidget<Widgets::Selection::CheckBox>();
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<bool>>();

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
    auto& widget = p_root.CreateWidget<Widgets::Drags::DragMultipleScalars<float, 2>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::array<float, 2>>>();

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
    auto& widget = p_root.CreateWidget<Widgets::Drags::DragMultipleScalars<float, 3>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::array<float, 3>>>();

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
    auto& widget = p_root.CreateWidget<Widgets::Drags::DragMultipleScalars<float, 4>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::array<float, 4>>>();

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
    auto& widget = p_root.CreateWidget<Widgets::Drags::DragMultipleScalars<float, 4>>(GetDataType<float>(), p_min, p_max, 0.f, p_step, "", GetFormat<float>());
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::array<float, 4>>>();

    dispatcher.RegisterGatherer([p_gatherer]()
                                {
				Maths::Quaternion value = p_gatherer();
				return reinterpret_cast<const std::array<float, 4>&>(value); });

    dispatcher.RegisterProvider([&dispatcher, p_provider](std::array<float, 4> p_value)
                                { p_provider(Maths::Quaternion::Normalize(reinterpret_cast<Maths::Quaternion&>(p_value))); });
}

void GUIDrawer::DrawDDString(Internal::WidgetContainer& p_root, const std::string& p_name,
                             std::function<std::string()> p_gatherer, std::function<void(std::string)> p_provider,
                             const std::string& p_identifier)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::InputFields::InputText>("");
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::string>>();
    dispatcher.RegisterGatherer(p_gatherer);
    dispatcher.RegisterProvider(p_provider);

    auto& ddTarget = widget.AddPlugin<Plugins::DDTarget<std::pair<std::string, Widgets::Layout::Group*>>>(p_identifier);
    ddTarget.DataReceivedEvent += [&widget, &dispatcher](std::pair<std::string, Widgets::Layout::Group*> p_data)
    {
        widget.content = p_data.first;
        dispatcher.NotifyChange();
    };
}

void GUIDrawer::DrawString(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<std::string(void)> p_gatherer, std::function<void(std::string)> p_provider)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::InputFields::InputText>("");
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<std::string>>();
    dispatcher.RegisterGatherer(p_gatherer);
    dispatcher.RegisterProvider(p_provider);
}

void GUIDrawer::DrawColor(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Color(void)> p_gatherer, std::function<void(Maths::Color)> p_provider, bool p_hasAlpha)
{
    CreateTitle(p_root, p_name);
    auto& widget = p_root.CreateWidget<Widgets::Selection::ColorEdit>(p_hasAlpha);
    auto& dispatcher = widget.AddPlugin<Plugins::DataDispatcher<Maths::Color>>();
    dispatcher.RegisterGatherer(p_gatherer);
    dispatcher.RegisterProvider(p_provider);
}

} // namespace NLS::UI
