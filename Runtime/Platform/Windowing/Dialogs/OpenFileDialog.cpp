#include "Windowing/Dialogs/OpenFileDialog.h"

#include <Windows.h>

NLS::Dialogs::OpenFileDialog::OpenFileDialog(const std::string & p_dialogTitle) : FileDialog(GetOpenFileNameA, p_dialogTitle)
{
}

void NLS::Dialogs::OpenFileDialog::AddFileType(const std::string & p_label, const std::string & p_filter)
{
	m_filter += p_label + '\0' + p_filter + '\0';
}
