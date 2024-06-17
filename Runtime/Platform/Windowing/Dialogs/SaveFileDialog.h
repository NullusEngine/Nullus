#pragma once

#include "PlatformDef.h"
#include "portable-file-dialogs.h"

namespace NLS::Dialogs
{
	/**
	* Dialog window that asks the user to save a file to the disk
	*/
	class NLS_PLATFORM_API SaveFileDialog
	{
    public:
        SaveFileDialog(std::string const& title,
                       std::string const& initial_path = "",
                       std::vector<std::string> filters = {"All Files", "*"},
                       bool confirm_overwrite = false);
        std::string Result();
        bool Ready(int timeout = 20) const;
        bool Kill() const;

    private:
        pfd::save_file sv;
	};
}