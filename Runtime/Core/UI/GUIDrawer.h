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



namespace NLS::Rendering::Data
{
class Material;
}

namespace NLS::Rendering::Resources
{
	class Model;
	class Shader;
	class Texture;
    }

namespace NLS::UI
{
	/**
	* Provide some helpers to draw UI elements
	*/
	class GUIDrawer
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
		static void ProvideEmptyTexture(Rendering::Resources::Texture& p_emptyTexture);

		/**
		* Draw a title with the title color
		* @param p_root
		* @param p_name
		*/
		static void CreateTitle(UI::Internal::WidgetContainer& p_root, const std::string& p_name);
	
		template <typename T>
		static void DrawScalar(Internal::WidgetContainer& p_root, const std::string& p_name, T& p_data, float p_step = 1.f, T p_min = std::numeric_limits<T>::min(), T p_max = std::numeric_limits<T>::max());
		static void DrawBoolean(Internal::WidgetContainer& p_root, const std::string& p_name, bool& p_data);
		static void DrawVec2(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector2& p_data, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
		static void DrawVec3(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector3& p_data, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
		static void DrawVec4(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Vector4& p_data, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
		static void DrawQuat(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Quaternion& p_data, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
		static void DrawString(Internal::WidgetContainer& p_root, const std::string& p_name, std::string& p_data);
		static void DrawColor(Internal::WidgetContainer& p_root, const std::string& p_name, Maths::Color& p_color, bool p_hasAlpha = false);
		static Widgets::Texts::Text& DrawMesh(Internal::WidgetContainer& p_root, const std::string& p_name, Rendering::Resources::Model*& p_data, Event<>* p_updateNotifier = nullptr);
		static Widgets::Visual::Image& DrawTexture(Internal::WidgetContainer& p_root, const std::string& p_name, Rendering::Resources::Texture*& p_data, Event<>* p_updateNotifier = nullptr);
		static Widgets::Texts::Text& DrawShader(Internal::WidgetContainer& p_root, const std::string& p_name, Rendering::Resources::Shader*& p_data, Event<>* p_updateNotifier = nullptr);
        static Widgets::Texts::Text& DrawMaterial(Internal::WidgetContainer& p_root, const std::string& p_name, NLS::Rendering::Data::Material*& p_data, Event<>* p_updateNotifier = nullptr);
		//static Widgets::Texts::Text& DrawSound(Internal::WidgetContainer& p_root, const std::string& p_name, OvAudio::Resources::Sound*& p_data, Event<>* p_updateNotifier = nullptr);
		static Widgets::Texts::Text& DrawAsset(Internal::WidgetContainer& p_root, const std::string& p_name, std::string& p_data, Event<>* p_updateNotifier = nullptr);

		template <typename T>
		static void DrawScalar(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<T(void)> p_gatherer, std::function<void(T)> p_provider, float p_step = 1.f, T p_min = std::numeric_limits<T>::min(), T p_max = std::numeric_limits<T>::max());
		static void DrawBoolean(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<bool(void)> p_gatherer, std::function<void(bool)> p_provider);
		static void DrawVec2(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Vector2(void)> p_gatherer, std::function<void(Maths::Vector2)> p_provider, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
		static void DrawVec3(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Vector3(void)> p_gatherer, std::function<void(Maths::Vector3)> p_provider, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
		static void DrawVec4(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Vector4(void)> p_gatherer, std::function<void(Maths::Vector4)> p_provider, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
		static void DrawQuat(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Quaternion(void)> p_gatherer, std::function<void(Maths::Quaternion)> p_provider, float p_step = 1.f, float p_min = _MIN_FLOAT, float p_max = _MAX_FLOAT);
		static void DrawDDString(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<std::string(void)> p_gatherer, std::function<void(std::string)> p_provider, const std::string& p_identifier);
		static void DrawString(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<std::string(void)> p_gatherer, std::function<void(std::string)> p_provider);
        static void DrawColor(Internal::WidgetContainer& p_root, const std::string& p_name, std::function<Maths::Color(void)> p_gatherer, std::function<void(Maths::Color)> p_provider, bool p_hasAlpha = false);

		template <typename T>
		static ImGuiDataType_ GetDataType();

		template <typename T>
		static std::string GetFormat();

	private:
		static Rendering::Resources::Texture* __EMPTY_TEXTURE;
	};
}

#include "GUIDrawer.inl"