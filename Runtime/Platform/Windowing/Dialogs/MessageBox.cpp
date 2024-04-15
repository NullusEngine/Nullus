
#if defined(_WIN32)
#include <windows.h>
#endif
#include "Windowing/Dialogs/MessageBox.h"

#undef MessageBox

NLS::Dialogs::MessageBox::MessageBox(std::string p_title, std::string p_message, EMessageType p_messageType, EButtonLayout p_buttonLayout, bool p_autoSpawn) :
	m_title(p_title),
	m_message(p_message),
	m_buttonLayout(p_buttonLayout),
	m_messageType(p_messageType)
{
	if (p_autoSpawn)
		Spawn();
}

const NLS::Dialogs::MessageBox::EUserAction& NLS::Dialogs::MessageBox::GetUserAction() const
{
	return m_userResult;
}

void NLS::Dialogs::MessageBox::Spawn()
{
	#if defined(_WIN32)
	int msgboxID = MessageBoxA
	(
		nullptr,
		static_cast<LPCSTR>(m_message.c_str()),
		static_cast<LPCSTR>(m_title.c_str()),
		static_cast<UINT>(m_messageType) | static_cast<UINT>(m_buttonLayout) | MB_DEFBUTTON2
	);

	m_userResult = static_cast<EUserAction>(msgboxID);
	#endif
}
