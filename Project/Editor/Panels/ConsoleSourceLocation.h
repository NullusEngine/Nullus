#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <utility>

namespace NLS::Editor::Panels
{
struct ConsoleSourceLocation
{
    std::string path;
    int line = 0;
};

inline std::filesystem::path ResolveConsoleSourcePath(
    const std::filesystem::path& source, const std::filesystem::path& projectRoot)
{
    if (source.empty() || source.is_absolute() || projectRoot.empty()) return source;
    const auto candidate = (projectRoot / source).lexically_normal();
    std::error_code error;
    return std::filesystem::is_regular_file(candidate, error) ? candidate : source;
}

inline ConsoleSourceLocation ParseConsoleStackLocation(const std::string& text)
{
    size_t delimiter = text.rfind(":line ");
    size_t number = delimiter == std::string::npos ? std::string::npos : delimiter + 6;
    if (delimiter == std::string::npos)
    {
        for (size_t i = text.size(); i-- > 0;)
        {
            if (text[i] == ':' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])))
            {
                delimiter = i;
                number = i + 1;
                break;
            }
        }
    }
    if (delimiter == std::string::npos || number == std::string::npos) return {};
    int line = 0;
    while (number < text.size() && std::isdigit(static_cast<unsigned char>(text[number])))
    {
        line = std::min(100000000, line * 10 + (text[number] - '0'));
        ++number;
    }
    if (line <= 0) return {};
    const auto luaStart = text.find("[string \"");
    if (luaStart != std::string::npos)
    {
        const auto pathStart = luaStart + 9;
        const auto pathEnd = text.find("\"]", pathStart);
        if (pathEnd != std::string::npos && pathEnd <= delimiter)
            return {text.substr(pathStart, pathEnd - pathStart), line};
    }
    size_t start = 0;
    const auto in = text.rfind(" in ", delimiter);
    const auto paren = text.rfind('(', delimiter);
    if (in != std::string::npos) start = in + 4;
    else if (paren != std::string::npos) start = paren + 1;
    else
    {
        const auto quote = text.rfind('"', delimiter);
        if (quote != std::string::npos) start = quote + 1;
        while (start < delimiter && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    }
    auto path = text.substr(start, delimiter - start);
    while (!path.empty() && (path.back() == '"' || std::isspace(static_cast<unsigned char>(path.back())))) path.pop_back();
    if (path.empty()) return {};
    return {std::move(path), line};
}
}
