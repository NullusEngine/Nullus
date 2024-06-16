#pragma once

#include <string>
#include <functional>
#include "PlatformDef.h"

struct tagOFNA;

namespace NLS::Dialogs
{
	/**
	* 一些可以传递给 FileDialog 实例的标志
	*/
	enum class NLS_PLATFORM_API EExplorerFlags
	{
		READONLY                 = 0x00000001,
		OVERWRITEPROMPT          = 0x00000002,
		HIDEREADONLY             = 0x00000004,
		NOCHANGEDIR              = 0x00000008,
		SHOWHELP                 = 0x00000010,
		ENABLEHOOK               = 0x00000020,
		ENABLETEMPLATE           = 0x00000040,
		ENABLETEMPLATEHANDLE     = 0x00000080,
		NOVALIDATE               = 0x00000100,
		ALLOWMULTISELECT         = 0x00000200,
		EXTENSIONDIFFERENT       = 0x00000400,
		PATHMUSTEXIST            = 0x00000800,
		FILEMUSTEXIST            = 0x00001000,
		CREATEPROMPT             = 0x00002000,
		SHAREAWARE               = 0x00004000,
		NOREADONLYRETURN         = 0x00008000,
		NOTESTFILECREATE         = 0x00010000,
		NONETWORKBUTTON          = 0x00020000,
		NOLONGNAMES              = 0x00040000,	// force no long names for 4.x modules
		EXPLORER                 = 0x00080000,	// new look commdlg
		NODEREFERENCELINKS       = 0x00100000,	
		LONGNAMES                = 0x00200000,	// force long names for 3.x modules
		ENABLEINCLUDENOTIFY      = 0x00400000,	// send include message to callback
		ENABLESIZING             = 0x00800000,	
		DONTADDTORECENT          = 0x02000000,	
		FORCESHOWHIDDEN          = 0x10000000	// Show All files including System and hidden files
	};

	inline EExplorerFlags operator~ (EExplorerFlags a) { return (EExplorerFlags)~(int)a; }
	inline EExplorerFlags operator| (EExplorerFlags a, EExplorerFlags b) { return (EExplorerFlags)((int)a | (int)b); }
	inline EExplorerFlags operator& (EExplorerFlags a, EExplorerFlags b) { return (EExplorerFlags)((int)a & (int)b); }
	inline EExplorerFlags operator^ (EExplorerFlags a, EExplorerFlags b) { return (EExplorerFlags)((int)a ^ (int)b); }
	inline EExplorerFlags& operator|= (EExplorerFlags& a, EExplorerFlags b) { return (EExplorerFlags&)((int&)a |= (int)b); }
	inline EExplorerFlags& operator&= (EExplorerFlags& a, EExplorerFlags b) { return (EExplorerFlags&)((int&)a &= (int)b); }
	inline EExplorerFlags& operator^= (EExplorerFlags& a, EExplorerFlags b) { return (EExplorerFlags&)((int&)a ^= (int)b); }

	/**
	* FileDialog 是任何要求用户从磁盘选择文件/将文件保存到磁盘的对话框窗口的基类
	*/
	class NLS_PLATFORM_API FileDialog
	{
	public:
		/**
		* Constructor
		* @param p_callback
		* @param p_dialogTitle
		*/
		FileDialog(std::function<int(tagOFNA*)> p_callback, const std::string& p_dialogTitle);

		/**
		* 定义初始目录（文件对话框将在其中打开）
		* @param p_initalDirectory
		*/
		void SetInitialDirectory(const std::string& p_initialDirectory);

		/**
		* 显示文件对话框
		* @param p_flags
		*/
		virtual void Show(EExplorerFlags p_flags = EExplorerFlags::DONTADDTORECENT | EExplorerFlags::FILEMUSTEXIST | EExplorerFlags::HIDEREADONLY | EExplorerFlags::NOCHANGEDIR);

		/**
		* 如果文件操作成功则返回 true
		*/
		bool HasSucceeded() const;

		/**
		* 返回选定的文件名（在调用此方法之前确保 HasSucceeded() 返回 true）
		*/
		std::string GetSelectedFileName();

		/**
		* 返回选定的文件路径（在调用此方法之前确保 HasSucceeded() 返回 true）
		*/
		std::string GetSelectedFilePath();

		/**
		* 返回有关最后一个错误的一些信息（在调用此方法之前确保 HasSucceeded() 返回 false）
		*/
		std::string GetErrorInfo();

		/**
		* 如果所选文件存在则返回 true
		*/
		bool IsFileExisting() const;

	private:
		void HandleError();

	protected:
		std::function<int(tagOFNA*)> m_callback;
		const std::string m_dialogTitle;
		std::string m_initialDirectory;
		std::string m_filter;
		std::string m_error;
		std::string m_filename;
		std::string m_filepath;
		bool m_succeeded;
	};
}