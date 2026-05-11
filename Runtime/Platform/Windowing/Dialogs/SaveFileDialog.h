#pragma once

#include <memory>
#include <string>
#include <vector>

#include "PlatformDef.h"
#ifdef APIENTRY
#undef APIENTRY
#endif

namespace pfd
{
class save_file;
}

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
        ~SaveFileDialog();

        std::string Result();
        bool Ready(int timeout = 20) const;
        bool Kill() const;

    private:
        std::unique_ptr<pfd::save_file> sv;
	};
}
