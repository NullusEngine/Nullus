#include "Assets/ScriptAssetUtility.h"

#include "Assets/AssetMeta.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

namespace NLS::Editor::Assets
{
namespace
{
bool IsIdentifierStart(const char value)
{
    return (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') || value == '_';
}

bool IsIdentifierPart(const char value)
{
    return IsIdentifierStart(value) || (value >= '0' && value <= '9');
}

bool IsIdentifierToken(const std::string& token)
{
    if (token.empty() || !IsIdentifierStart(token.front()))
        return false;
    return std::all_of(
        token.begin() + 1,
        token.end(),
        [](const char value) { return IsIdentifierPart(value); });
}

std::vector<std::string> TokenizeScriptSource(std::string_view source)
{
    std::vector<std::string> tokens;
    size_t position = 0;

    const auto skipQuotedString = [&source](size_t& cursor)
    {
        const char quote = source[cursor];
        size_t quoteCount = 0;
        while (cursor + quoteCount < source.size() && source[cursor + quoteCount] == quote)
            ++quoteCount;

        if (quote == '"' && quoteCount >= 3)
        {
            cursor += quoteCount;
            while (cursor < source.size())
            {
                if (source[cursor] != '"')
                {
                    ++cursor;
                    continue;
                }

                size_t closingCount = 0;
                while (cursor + closingCount < source.size() &&
                    source[cursor + closingCount] == '"')
                {
                    ++closingCount;
                }
                if (closingCount >= quoteCount)
                {
                    cursor += closingCount;
                    return;
                }
                cursor += closingCount;
            }
            return;
        }

        ++cursor;
        while (cursor < source.size())
        {
            if (source[cursor] == '\\')
            {
                cursor += std::min<size_t>(2, source.size() - cursor);
                continue;
            }
            if (source[cursor++] == quote)
                return;
        }
    };

    const auto skipLongBracket = [&source](size_t& cursor)
    {
        if (cursor >= source.size() || source[cursor] != '[')
            return false;

        size_t opener = cursor + 1;
        while (opener < source.size() && source[opener] == '=')
            ++opener;
        if (opener >= source.size() || source[opener] != '[')
            return false;

        const size_t equals = opener - cursor - 1;
        cursor = opener + 1;
        while (cursor < source.size())
        {
            if (source[cursor] != ']')
            {
                ++cursor;
                continue;
            }

            size_t closer = cursor + 1;
            size_t count = 0;
            while (count < equals && closer < source.size() && source[closer] == '=')
            {
                ++count;
                ++closer;
            }
            if (count == equals && closer < source.size() && source[closer] == ']')
            {
                cursor = closer + 1;
                return true;
            }
            ++cursor;
        }

        return true;
    };

    while (position < source.size())
    {
        const char current = source[position];
        if (std::isspace(static_cast<unsigned char>(current)) != 0)
        {
            ++position;
            continue;
        }

        const bool csharpComment = current == '/' && position + 1 < source.size() &&
            (source[position + 1] == '/' || source[position + 1] == '*');
        const bool luaComment = current == '-' && position + 1 < source.size() &&
            source[position + 1] == '-';
        if (csharpComment || luaComment)
        {
            const bool longComment = luaComment && position + 2 < source.size() &&
                source[position + 2] == '[';
            position += 2;
            if (longComment && skipLongBracket(position))
                continue;

            const bool blockComment = csharpComment && source[position - 1] == '*';
            if (blockComment)
            {
                while (position + 1 < source.size() &&
                    !(source[position] == '*' && source[position + 1] == '/'))
                {
                    ++position;
                }
                position = std::min(source.size(), position + 2);
            }
            else
            {
                while (position < source.size() && source[position] != '\n')
                    ++position;
            }
            continue;
        }

        if (current == '\'' || current == '"')
        {
            skipQuotedString(position);
            continue;
        }

        if (current == '[' && skipLongBracket(position))
            continue;

        if (IsIdentifierStart(current))
        {
            const size_t start = position++;
            while (position < source.size() && IsIdentifierPart(source[position]))
                ++position;
            tokens.emplace_back(source.substr(start, position - start));
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(current)) != 0)
        {
            const size_t start = position++;
            while (position < source.size() &&
                (IsIdentifierPart(source[position]) || source[position] == '.'))
            {
                ++position;
            }
            tokens.emplace_back(source.substr(start, position - start));
            continue;
        }

        tokens.emplace_back(1, current);
        ++position;
    }

    return tokens;
}

bool IsCSharpBehaviourSource(const std::vector<std::string>& tokens)
{
    for (size_t index = 0; index < tokens.size(); ++index)
    {
        if (tokens[index] != "class" || index + 1 >= tokens.size())
            continue;

        bool isPublic = false;
        bool isAbstract = false;
        for (size_t cursor = index; cursor > 0; --cursor)
        {
            const auto& token = tokens[cursor - 1];
            if (token == "{" || token == "}" || token == ";")
                break;
            isPublic = isPublic || token == "public";
            isAbstract = isAbstract || token == "abstract";
        }
        if (!isPublic || isAbstract)
            continue;

        size_t baseStart = index + 2;
        while (baseStart < tokens.size() && tokens[baseStart] != ":" &&
            tokens[baseStart] != "{" && tokens[baseStart] != ";")
        {
            ++baseStart;
        }
        if (baseStart >= tokens.size() || tokens[baseStart] != ":")
            continue;

        for (size_t cursor = baseStart + 1; cursor < tokens.size(); ++cursor)
        {
            if (tokens[cursor] == "{" || tokens[cursor] == ";")
                break;
            if (tokens[cursor] == "Behaviour")
                return true;
        }
    }

    return false;
}

bool IsLuaModuleSource(const std::vector<std::string>& tokens)
{
    for (size_t index = 0; index + 1 < tokens.size(); ++index)
    {
        if (tokens[index] != "return")
            continue;

        if (IsIdentifierToken(tokens[index + 1]))
        {
            size_t after = index + 2;
            while (after < tokens.size() && tokens[after] == ";")
                ++after;
            if (after == tokens.size())
                return true;
            continue;
        }

        if (tokens[index + 1] != "{")
            continue;

        size_t depth = 0;
        size_t cursor = index + 1;
        for (; cursor < tokens.size(); ++cursor)
        {
            if (tokens[cursor] == "{")
                ++depth;
            else if (tokens[cursor] == "}" && --depth == 0)
                break;
        }
        if (cursor >= tokens.size())
            continue;
        ++cursor;
        while (cursor < tokens.size() && tokens[cursor] == ";")
            ++cursor;
        if (cursor == tokens.size())
            return true;
    }

    return false;
}

std::filesystem::path ProjectRelativePath(const std::string& value)
{
    auto path = std::filesystem::path(value).lexically_normal();
    if (path.has_root_name() || path.is_absolute())
        return path;

    const auto generic = path.generic_string();
    if (generic == "Assets")
        return {};
    if (generic.rfind("Assets/", 0u) == 0u)
        return std::filesystem::path(generic.substr(7u));
    return path;
}
}

bool IsScriptAssetPath(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension == ".cs" || extension == ".lua";
}

bool IsScriptComponentSource(
    const NLS::Scripting::ScriptLanguage language,
    const std::string_view source)
{
    const auto tokens = TokenizeScriptSource(source);
    switch (language)
    {
    case NLS::Scripting::ScriptLanguage::CSharp:
        return IsCSharpBehaviourSource(tokens);
    case NLS::Scripting::ScriptLanguage::Lua:
        return IsLuaModuleSource(tokens);
    case NLS::Scripting::ScriptLanguage::Unknown:
    default:
        return false;
    }
}

std::filesystem::path ResolveProjectScriptPath(
    const std::filesystem::path& projectAssetsFolder,
    const EditorAssetDragPayload& payload)
{
    const auto path = ProjectRelativePath(GetEditorAssetDragPayloadPath(payload));
    if (path.empty())
        return {};
    if (path.has_root_name() || path.is_absolute())
        return path.lexically_normal();
    return (projectAssetsFolder / path).lexically_normal();
}

std::optional<NLS::Scripting::ScriptAsset> LoadScriptAsset(
    const std::filesystem::path& projectAssetsFolder,
    const EditorAssetDragPayload& payload)
{
    if (!IsEditorAssetDragPayloadScript(payload))
        return std::nullopt;

    const auto absolutePath = ResolveProjectScriptPath(projectAssetsFolder, payload);
    if (absolutePath.empty() || !std::filesystem::is_regular_file(absolutePath))
        return std::nullopt;

    std::ifstream input(absolutePath, std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    const auto assetId = GetEditorAssetDragPayloadAssetId(payload);
    if (!assetId.IsValid())
        return std::nullopt;

    const auto resourcePath = GetEditorAssetDragPayloadPath(payload);
    auto extension = absolutePath.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    const auto language = extension == ".cs"
        ? NLS::Scripting::ScriptLanguage::CSharp
        : NLS::Scripting::ScriptLanguage::Lua;

    NLS::Scripting::ScriptAsset asset;
    asset.assetId = assetId;
    asset.language = language;
    asset.sourcePath = resourcePath;
    asset.sourceText = source;
    asset.scriptType = NLS::Scripting::MakeStableScriptId(
        std::string(language == NLS::Scripting::ScriptLanguage::CSharp ? "CSharp:" : "Lua:") + resourcePath);
    asset.contentHash = NLS::Scripting::MakeScriptContentHash(source);
    asset.isComponent = IsScriptComponentSource(language, source);
    return asset;
}

std::vector<NLS::Scripting::ScriptAsset> FindProjectScriptAssets(
    const std::filesystem::path& projectAssetsFolder)
{
    std::vector<NLS::Scripting::ScriptAsset> scripts;
    if (projectAssetsFolder.empty() || !std::filesystem::is_directory(projectAssetsFolder))
        return scripts;

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        projectAssetsFolder,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end && !error; iterator.increment(error))
    {
        const auto status = iterator->symlink_status(error);
        if (error)
        {
            error.clear();
            continue;
        }
        if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status))
            continue;

        const auto absolutePath = iterator->path().lexically_normal();
        if (!IsScriptAssetPath(absolutePath))
            continue;

        const auto meta = NLS::Core::Assets::AssetMeta::Load(
            NLS::Core::Assets::GetAssetMetaPath(absolutePath));
        if (!meta.has_value() || !meta->id.IsValid() ||
            meta->assetType != NLS::Core::Assets::AssetType::Script)
        {
            continue;
        }

        const auto relativePath = absolutePath.lexically_relative(projectAssetsFolder);
        if (relativePath.empty() || relativePath.is_absolute())
            continue;

        const auto editorPath = (std::filesystem::path("Assets") / relativePath).generic_string();
        const auto payload = MakeEditorAssetDragPayload(
            editorPath,
            meta->id,
            {},
            NLS::Core::Assets::ArtifactType::Unknown,
            false,
            true);
        if (auto asset = LoadScriptAsset(projectAssetsFolder, payload); asset.has_value())
            scripts.push_back(std::move(*asset));
    }

    std::sort(
        scripts.begin(),
        scripts.end(),
        [](const NLS::Scripting::ScriptAsset& left, const NLS::Scripting::ScriptAsset& right)
        {
            return left.sourcePath < right.sourcePath;
        });
    return scripts;
}
}
