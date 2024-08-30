#include <UI/Widgets/Texts/Text.h>
#include <UI/Panels/PanelWindow.h>

#include <Filesystem/IniFile.h>

namespace NLS::Editor::Panels
{
	class ProjectSettings : public UI::PanelWindow
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		ProjectSettings
		(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);

		/**
		* Generate a gatherer that will get the value associated to the given key
		* @param p_keyName
		*/
		template <typename T>
		std::function<T()> GenerateGatherer(const std::string& p_keyName)
		{
			return std::bind(&Filesystem::IniFile::Get<T>, &m_projectFile, p_keyName);
		}

		/**
		* Generate a provider that will set the value associated to the given key
		* @param p_keyName
		*/
		template <typename T>
		std::function<void(T)> GenerateProvider(const std::string& p_keyName)
		{
			return std::bind(&Filesystem::IniFile::Set<T>, &m_projectFile, p_keyName, std::placeholders::_1);
		}

	private:
		Filesystem::IniFile& m_projectFile;
	};
}