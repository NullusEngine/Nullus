#pragma once

#include "Windowing/Dialogs/FileDialog.h"

namespace NLS::Dialogs
{
	/**
	* Dialog window that asks the user to select a file from the disk
	*/
	class NLS_PLATFORM_API OpenFileDialog : public FileDialog
	{
	public:
		/**
		* Constructor
		* @param p_dialogTitle
		*/
		OpenFileDialog(const std::string& p_dialogTitle);

		/**
		* Add a supported file type to the dialog window
		* @param p_label
		* @param p_filter
		*/
		void AddFileType(const std::string& p_label, const std::string& p_filter);
	};
}