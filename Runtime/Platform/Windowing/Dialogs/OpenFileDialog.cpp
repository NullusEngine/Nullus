#include "Windowing/Dialogs/OpenFileDialog.h"
#if defined(_WIN32)
#include <Windows.h>
#endif
#if defined(_WIN32)
NLS::Dialogs::OpenFileDialog::OpenFileDialog(const std::string & p_dialogTitle) : FileDialog(GetOpenFileNameA, p_dialogTitle)
{
}
#else
NLS::Dialogs::OpenFileDialog::OpenFileDialog(const std::string & p_dialogTitle) : FileDialog(nullptr, p_dialogTitle)
{
}
#endif
void NLS::Dialogs::OpenFileDialog::AddFileType(const std::string & p_label, const std::string & p_filter)
{
	m_filter += p_label + '\0' + p_filter + '\0';
}
