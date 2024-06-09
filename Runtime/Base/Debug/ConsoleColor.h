#pragma once

#include <iostream>
#ifdef _WIN32
#include <windows.h>
#else
typedef unsigned short WORD;
#endif
#define COLOR_BLUE NLS::Debug::blue
#define COLOR_RED NLS::Debug::red
#define COLOR_GREEN NLS::Debug::green
#define COLOR_YELLOW NLS::Debug::yellow
#define COLOR_WHITE NLS::Debug::white
#define COLOR_DEFAULT NLS::Debug::grey

namespace NLS::Debug
{
	inline std::ostream& blue(std::ostream& s)
	{
#ifdef _WIN32
		HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
		SetConsoleTextAttribute(hStdout,
			FOREGROUND_BLUE | FOREGROUND_GREEN |
			FOREGROUND_INTENSITY);
#endif
		return s;
	}

	inline std::ostream& red(std::ostream& s)
	{
#ifdef _WIN32
		HANDLE hStdout = GetStdHandle(STD_ERROR_HANDLE);
		SetConsoleTextAttribute(hStdout,
			FOREGROUND_RED | FOREGROUND_INTENSITY);
#endif
		return s;
	}

	inline std::ostream& green(std::ostream& s)
	{
#ifdef _WIN32
		HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
		SetConsoleTextAttribute(hStdout,
			FOREGROUND_GREEN | FOREGROUND_INTENSITY);
#endif
		return s;
	}

	inline std::ostream& yellow(std::ostream& s)
	{
#ifdef _WIN32
		HANDLE hStdout = GetStdHandle(STD_ERROR_HANDLE);
		SetConsoleTextAttribute(hStdout,
			FOREGROUND_GREEN | FOREGROUND_RED |
			FOREGROUND_INTENSITY);
#endif
		return s;
	}

	inline std::ostream& white(std::ostream& s)
	{
#ifdef _WIN32
		HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
		SetConsoleTextAttribute(hStdout,
			FOREGROUND_RED | FOREGROUND_GREEN |
			FOREGROUND_BLUE | FOREGROUND_INTENSITY);
#endif
		return s;
	}

	inline std::ostream& grey(std::ostream& s)
	{
#ifdef _WIN32
		HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
		SetConsoleTextAttribute(hStdout,
			FOREGROUND_RED | FOREGROUND_GREEN |
			FOREGROUND_BLUE);
#endif
		return s;
	}

	struct color
	{
		color(WORD attribute) : m_color(attribute)
		{
		}

		WORD m_color;
	};

	template <class _Elem, class _Traits>
	std::basic_ostream<_Elem, _Traits>&
		operator<<(std::basic_ostream<_Elem, _Traits>& i, color& c)
	{
#ifdef _WIN32
		HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
		SetConsoleTextAttribute(hStdout, c.m_color);
#endif
		return i;
	}
}