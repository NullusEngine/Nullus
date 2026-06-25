#include "Rendering/ShaderLab/ShaderLabParser.h"

#include "Rendering/ShaderLab/ShaderLabAsset.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <sstream>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace NLS::Render::ShaderLab
{
    namespace
    {
        enum class TokenKind : uint8_t
        {
            Identifier,
            String,
            Number,
            LBrace,
            RBrace,
            LParen,
            RParen,
            Comma,
            Equals,
            End
        };

        struct Token
        {
            TokenKind kind = TokenKind::End;
            std::string text;
            ShaderLabSourceLocation location;
        };

        class Parser
        {
        public:
            Parser(std::string_view source, std::string filePath)
                : m_source(source)
                , m_filePath(std::move(filePath))
            {
                m_current = LexToken();
            }

            ShaderLabParseResult Parse()
            {
                ShaderLabParseResult result;
                m_result = &result;

                ExpectIdentifier("Shader");
                result.asset.shaderName = ExpectString("expected shader name");
                Expect(TokenKind::LBrace, "expected '{' after Shader name");

                while (!AtEnd() && !Check(TokenKind::RBrace))
                {
                    if (MatchIdentifier("Properties"))
                        ParseProperties(result.asset);
                    else if (MatchIdentifier("SubShader"))
                        ParseSubShader(result.asset);
                    else if (MatchIdentifier("Fallback"))
                        result.asset.fallbackShader = ExpectString("expected fallback shader name");
                    else
                        Error(m_current.location, "unexpected token '" + m_current.text + "'");
                }

                Expect(TokenKind::RBrace, "expected '}' to close Shader");
                m_result = nullptr;
                return result;
            }

        private:
            bool AtEnd() const { return m_current.kind == TokenKind::End; }
            bool Check(const TokenKind kind) const { return m_current.kind == kind; }

            bool IsIdentifier(std::string_view value) const
            {
                return m_current.kind == TokenKind::Identifier && m_current.text == value;
            }

            bool MatchIdentifier(std::string_view value)
            {
                if (!IsIdentifier(value))
                    return false;
                Advance();
                return true;
            }

            void ExpectIdentifier(std::string_view value)
            {
                if (MatchIdentifier(value))
                    return;
                Error(m_current.location, "expected '" + std::string(value) + "'");
            }

            bool Match(const TokenKind kind)
            {
                if (!Check(kind))
                    return false;
                Advance();
                return true;
            }

            Token Expect(const TokenKind kind, std::string message)
            {
                const Token token = m_current;
                if (Match(kind))
                    return token;
                Error(token.location, std::move(message));
                return token;
            }

            std::string ExpectString(std::string message)
            {
                const auto token = Expect(TokenKind::String, std::move(message));
                return token.text;
            }

            std::string ExpectIdentifierText(std::string message)
            {
                const auto token = Expect(TokenKind::Identifier, std::move(message));
                return token.text;
            }

            Token ExpectIdentifierToken(std::string message)
            {
                return Expect(TokenKind::Identifier, std::move(message));
            }

            Token ExpectIdentifierOrNumberToken(std::string message)
            {
                const Token token = m_current;
                if (Match(TokenKind::Identifier) || Match(TokenKind::Number))
                    return token;
                Error(token.location, std::move(message));
                return token;
            }

            float ExpectNumber(std::string message)
            {
                const auto token = Expect(TokenKind::Number, std::move(message));
                float value = 0.0f;
                const auto* begin = token.text.data();
                const auto* end = begin + token.text.size();
                const auto [ptr, ec] = std::from_chars(begin, end, value);
                if (ec != std::errc{} || ptr != end)
                    AddDiagnostic(token.location, "invalid numeric literal '" + token.text + "'");
                return value;
            }

            void Advance()
            {
                m_current = LexToken();
            }

            void Error(ShaderLabSourceLocation location, std::string message)
            {
                AddDiagnostic(std::move(location), std::move(message));
                if (!AtEnd())
                    Advance();
            }

            void AddDiagnostic(ShaderLabSourceLocation location, std::string message)
            {
                if (m_result != nullptr)
                    m_result->diagnostics.push_back({ std::move(message), std::move(location) });
            }

            void ParseProperties(ShaderLabAssetDesc& asset)
            {
                Expect(TokenKind::LBrace, "expected '{' after Properties");
                std::unordered_set<std::string> names;
                for (const auto& property : asset.properties)
                    names.insert(property.name);

                while (!AtEnd() && !Check(TokenKind::RBrace))
                {
                    const auto nameToken = Expect(TokenKind::Identifier, "expected property name");
                    ShaderLabPropertyDesc property;
                    property.name = nameToken.text;
                    property.location = nameToken.location;

                    Expect(TokenKind::LParen, "expected '(' after property name");
                    property.displayName = ExpectString("expected property display name");
                    Expect(TokenKind::Comma, "expected ',' before property type");
                    const auto typeToken = Expect(TokenKind::Identifier, "expected property type");
                    property.type = ParsePropertyType(typeToken.text, typeToken.location);

                    if (property.type == ShaderLabPropertyType::Range)
                    {
                        Expect(TokenKind::LParen, "expected '(' after Range");
                        property.rangeMin = ExpectNumber("expected range minimum");
                        Expect(TokenKind::Comma, "expected ',' in Range");
                        property.rangeMax = ExpectNumber("expected range maximum");
                        Expect(TokenKind::RParen, "expected ')' after Range");
                    }
                    Expect(TokenKind::RParen, "expected ')' after property declaration");
                    Expect(TokenKind::Equals, "expected '=' before property default");
                    property.defaultValue = ParsePropertyDefault(property.type);

                    if (!names.insert(property.name).second)
                    {
                        AddDiagnostic(property.location, "duplicate property '" + property.name + "'");
                    }
                    else
                    {
                        asset.properties.push_back(std::move(property));
                    }
                }
                Expect(TokenKind::RBrace, "expected '}' to close Properties");
            }

            ShaderLabPropertyType ParsePropertyType(const std::string& text, const ShaderLabSourceLocation& location)
            {
                if (text == "Float") return ShaderLabPropertyType::Float;
                if (text == "Int") return ShaderLabPropertyType::Int;
                if (text == "Range") return ShaderLabPropertyType::Range;
                if (text == "Vector") return ShaderLabPropertyType::Vector;
                if (text == "Color") return ShaderLabPropertyType::Color;
                if (text == "Texture2D") return ShaderLabPropertyType::Texture2D;
                if (text == "TextureCube") return ShaderLabPropertyType::TextureCube;
                AddDiagnostic(location, "unsupported property type '" + text + "'");
                return ShaderLabPropertyType::Float;
            }

            ShaderLabValueVariant ParsePropertyDefault(const ShaderLabPropertyType type)
            {
                switch (type)
                {
                case ShaderLabPropertyType::Int:
                {
                    const auto token = Expect(TokenKind::Number, "expected integer default");
                    int64_t value = 0;
                    const auto* begin = token.text.data();
                    const auto* end = begin + token.text.size();
                    const auto [ptr, ec] = std::from_chars(begin, end, value);
                    if (ec != std::errc{} || ptr != end || token.text.find_first_of(".eE") != std::string::npos)
                    {
                        AddDiagnostic(token.location, "expected integer literal for Int default");
                        return int32_t{0};
                    }
                    if (value < std::numeric_limits<int32_t>::min() ||
                        value > std::numeric_limits<int32_t>::max())
                    {
                        AddDiagnostic(token.location, "integer literal out of Int range");
                        return int32_t{0};
                    }
                    return static_cast<int32_t>(value);
                }
                case ShaderLabPropertyType::Float:
                case ShaderLabPropertyType::Range:
                    return ExpectNumber("expected numeric default");
                case ShaderLabPropertyType::Vector:
                case ShaderLabPropertyType::Color:
                    return ParseFloat4Default();
                case ShaderLabPropertyType::Texture2D:
                case ShaderLabPropertyType::TextureCube:
                    return ExpectString("expected texture default name");
                default:
                    return {};
                }
            }

            ShaderLabFloat4 ParseFloat4Default()
            {
                ShaderLabFloat4 value{ 0.0f, 0.0f, 0.0f, 0.0f };
                Expect(TokenKind::LParen, "expected '(' before vector default");
                value[0] = ExpectNumber("expected vector component");
                Expect(TokenKind::Comma, "expected ',' in vector default");
                value[1] = ExpectNumber("expected vector component");
                Expect(TokenKind::Comma, "expected ',' in vector default");
                value[2] = ExpectNumber("expected vector component");
                Expect(TokenKind::Comma, "expected ',' in vector default");
                value[3] = ExpectNumber("expected vector component");
                Expect(TokenKind::RParen, "expected ')' after vector default");
                return value;
            }

            void ParseSubShader(ShaderLabAssetDesc& asset)
            {
                ShaderLabSubShaderDesc subShader;
                Expect(TokenKind::LBrace, "expected '{' after SubShader");
                while (!AtEnd() && !Check(TokenKind::RBrace))
                {
                    if (MatchIdentifier("Tags"))
                        subShader.tags = ParseTags();
                    else if (MatchIdentifier("Pass"))
                        subShader.passes.push_back(ParsePass(asset.subShaders.size(), subShader.passes.size()));
                    else
                        Error(m_current.location, "unexpected token in SubShader '" + m_current.text + "'");
                }
                Expect(TokenKind::RBrace, "expected '}' to close SubShader");
                asset.subShaders.push_back(std::move(subShader));
            }

            ShaderLabTagSet ParseTags()
            {
                ShaderLabTagSet tags;
                Expect(TokenKind::LBrace, "expected '{' after Tags");
                while (!AtEnd() && !Check(TokenKind::RBrace))
                {
                    const auto keyToken = Expect(TokenKind::String, "expected tag key");
                    Expect(TokenKind::Equals, "expected '=' in tag");
                    const auto value = ExpectString("expected tag value");
                    if (tags.values.find(keyToken.text) != tags.values.end())
                    {
                        AddDiagnostic(keyToken.location, "duplicate tag '" + keyToken.text + "'");
                    }
                    else
                    {
                        tags.values[keyToken.text] = value;
                    }
                }
                Expect(TokenKind::RBrace, "expected '}' to close Tags");
                return tags;
            }

            ShaderLabPassDesc ParsePass(const size_t subShaderIndex, const size_t passIndex)
            {
                ShaderLabPassDesc pass;
                pass.subShaderIndex = static_cast<uint32_t>(subShaderIndex);
                pass.passIndex = static_cast<uint32_t>(passIndex);

                Expect(TokenKind::LBrace, "expected '{' after Pass");
                while (!AtEnd() && !Check(TokenKind::RBrace))
                {
                    if (MatchIdentifier("Name"))
                        pass.name = ExpectString("expected pass name");
                    else if (MatchIdentifier("Tags"))
                        pass.tags = ParseTags();
                    else if (MatchIdentifier("Cull"))
                    {
                        const auto token = ExpectIdentifierToken("expected Cull mode");
                        pass.state.cullMode = ParseCullMode(token.text, token.location);
                    }
                    else if (MatchIdentifier("ZWrite"))
                    {
                        const auto token = ExpectIdentifierOrNumberToken("expected ZWrite mode");
                        pass.state.depthWrite = ParseOnOff(token.text, token.location);
                    }
                    else if (MatchIdentifier("ZTest"))
                    {
                        const auto token = ExpectIdentifierToken("expected ZTest mode");
                        pass.state.depthCompare = ParseCompare(token.text, token.location);
                    }
                    else if (MatchIdentifier("Blend"))
                        ParseBlend(pass.state.blend);
                    else if (IsIdentifier("HLSLPROGRAM"))
                        ParseHlslBlock(pass);
                    else
                        Error(m_current.location, "unexpected token in Pass '" + m_current.text + "'");
                }
                Expect(TokenKind::RBrace, "expected '}' to close Pass");
                return pass;
            }

            ShaderLabCullMode ParseCullMode(const std::string& text, const ShaderLabSourceLocation& location)
            {
                if (text == "Off") return ShaderLabCullMode::Off;
                if (text == "Front") return ShaderLabCullMode::Front;
                if (text == "Back") return ShaderLabCullMode::Back;
                AddDiagnostic(location, "unsupported Cull mode '" + text + "'");
                return ShaderLabCullMode::Back;
            }

            bool ParseOnOff(const std::string& text, const ShaderLabSourceLocation& location)
            {
                if (text == "On" || text == "True" || text == "1")
                    return true;
                if (text == "Off" || text == "False" || text == "0")
                    return false;
                AddDiagnostic(location, "unsupported ZWrite mode '" + text + "'");
                return false;
            }

            NLS::Render::Settings::EComparaisonAlgorithm ParseCompare(
                const std::string& text,
                const ShaderLabSourceLocation& location)
            {
                using NLS::Render::Settings::EComparaisonAlgorithm;
                if (text == "Never") return EComparaisonAlgorithm::NEVER;
                if (text == "Less") return EComparaisonAlgorithm::LESS;
                if (text == "Equal") return EComparaisonAlgorithm::EQUAL;
                if (text == "LessEqual" || text == "LEqual") return EComparaisonAlgorithm::LESS_EQUAL;
                if (text == "Greater") return EComparaisonAlgorithm::GREATER;
                if (text == "NotEqual") return EComparaisonAlgorithm::NOTEQUAL;
                if (text == "GreaterEqual" || text == "GEqual") return EComparaisonAlgorithm::GREATER_EQUAL;
                if (text == "Always") return EComparaisonAlgorithm::ALWAYS;
                AddDiagnostic(location, "unsupported ZTest mode '" + text + "'");
                return EComparaisonAlgorithm::LESS;
            }

            NLS::Render::RHI::RHIBlendFactor ParseBlendFactor(
                const std::string& text,
                const ShaderLabSourceLocation& location)
            {
                using NLS::Render::RHI::RHIBlendFactor;
                if (text == "Zero") return RHIBlendFactor::Zero;
                if (text == "One") return RHIBlendFactor::One;
                if (text == "SrcColor") return RHIBlendFactor::SrcColor;
                if (text == "OneMinusSrcColor" || text == "InvSrcColor") return RHIBlendFactor::InvSrcColor;
                if (text == "SrcAlpha") return RHIBlendFactor::SrcAlpha;
                if (text == "OneMinusSrcAlpha" || text == "InvSrcAlpha") return RHIBlendFactor::InvSrcAlpha;
                if (text == "DstAlpha") return RHIBlendFactor::DstAlpha;
                if (text == "OneMinusDstAlpha" || text == "InvDstAlpha") return RHIBlendFactor::InvDstAlpha;
                if (text == "DstColor") return RHIBlendFactor::DstColor;
                if (text == "OneMinusDstColor" || text == "InvDstColor") return RHIBlendFactor::InvDstColor;
                AddDiagnostic(location, "unsupported Blend factor '" + text + "'");
                return RHIBlendFactor::One;
            }

            void ParseBlend(NLS::Render::RHI::RHIBlendStateDesc& blend)
            {
                if (MatchIdentifier("Off"))
                {
                    blend.enabled = false;
                    blend.renderTargets.clear();
                    return;
                }

                const auto src = ExpectIdentifierToken("expected source blend factor");
                const auto dst = ExpectIdentifierToken("expected destination blend factor");
                blend.enabled = true;
                blend.renderTargets.resize(1);
                blend.renderTargets[0].blendEnable = true;
                blend.renderTargets[0].srcColor = ParseBlendFactor(src.text, src.location);
                blend.renderTargets[0].dstColor = ParseBlendFactor(dst.text, dst.location);
                blend.renderTargets[0].srcAlpha = ParseBlendFactor(src.text, src.location);
                blend.renderTargets[0].dstAlpha = ParseBlendFactor(dst.text, dst.location);
            }

            void ParseHlslBlock(ShaderLabPassDesc& pass)
            {
                const auto hlslToken = m_current;
                const size_t programEnd = FindLineEnd(m_offset);
                const size_t rawStart = programEnd < m_source.size() ? programEnd + 1 : programEnd;
                const ShaderLabSourceLocation rawLocation = LocationAt(rawStart);
                const size_t end = FindEndHlsl(rawStart);
                if (end == std::string_view::npos)
                {
                    Error(hlslToken.location, "missing ENDHLSL for HLSLPROGRAM block");
                    m_offset = m_source.size();
                    m_current = LexToken();
                    return;
                }

                pass.hlslLocation = rawLocation;
                pass.hlslSource = std::string(m_source.substr(rawStart, end - rawStart));
                ExtractPragmas(pass);

                const size_t endLineEnd = FindLineEnd(end);
                m_offset = endLineEnd < m_source.size() ? endLineEnd + 1 : endLineEnd;
                m_current = LexToken();
            }

            void ExtractPragmas(ShaderLabPassDesc& pass)
            {
                std::istringstream stream(pass.hlslSource);
                std::string line;
                uint32_t lineOffset = 0;
                bool inBlockComment = false;
                while (std::getline(stream, line))
                {
                    const auto pragma = FindPragmaDirective(line, inBlockComment);
                    if (pragma.has_value())
                    {
                        std::istringstream pragmaStream(line.substr(*pragma + 7));
                        std::string kind;
                        pragmaStream >> kind;
                        if (kind == "vertex")
                            pragmaStream >> pass.vertexEntry;
                        else if (kind == "fragment")
                            pragmaStream >> pass.fragmentEntry;
                        else if (kind == "compute")
                            pragmaStream >> pass.computeEntry;
                        else if (kind == "shader_feature" || kind == "multi_compile")
                        {
                            ShaderLabKeywordPragma keywordPragma;
                            keywordPragma.location = pass.hlslLocation;
                            keywordPragma.location.line += lineOffset;
                            std::string keyword;
                            while (pragmaStream >> keyword)
                                keywordPragma.keywords.push_back(keyword);

                            if (kind == "shader_feature")
                                pass.shaderFeatures.push_back(std::move(keywordPragma));
                            else
                                pass.multiCompiles.push_back(std::move(keywordPragma));
                        }
                    }
                    ++lineOffset;
                }
            }

            std::optional<size_t> FindPragmaDirective(std::string_view line, bool& inBlockComment) const
            {
                bool inString = false;
                for (size_t i = 0; i < line.size(); ++i)
                {
                    const char c = line[i];
                    if (inBlockComment)
                    {
                        if (c == '*' && i + 1 < line.size() && line[i + 1] == '/')
                        {
                            inBlockComment = false;
                            ++i;
                        }
                        continue;
                    }

                    if (inString)
                    {
                        if (c == '\\' && i + 1 < line.size())
                        {
                            ++i;
                            continue;
                        }
                        if (c == '"')
                            inString = false;
                        continue;
                    }

                    if (c == '/' && i + 1 < line.size())
                    {
                        if (line[i + 1] == '/')
                            return std::nullopt;
                        if (line[i + 1] == '*')
                        {
                            inBlockComment = true;
                            ++i;
                            continue;
                        }
                    }

                    if (c == '"')
                    {
                        inString = true;
                        continue;
                    }

                    if (c == '#' && line.substr(i, 7) == "#pragma")
                        return i;
                }
                return std::nullopt;
            }

            size_t FindLineEnd(const size_t start) const
            {
                const size_t newline = m_source.find('\n', start);
                return newline == std::string_view::npos ? m_source.size() : newline;
            }

            size_t FindEndHlsl(const size_t start) const
            {
                size_t cursor = start;
                while (cursor < m_source.size())
                {
                    const size_t lineEnd = FindLineEnd(cursor);
                    auto line = m_source.substr(cursor, lineEnd - cursor);
                    line = Trim(line);
                    if (line == "ENDHLSL")
                        return cursor;
                    cursor = lineEnd < m_source.size() ? lineEnd + 1 : lineEnd;
                }
                return std::string_view::npos;
            }

            Token LexToken()
            {
                SkipWhitespaceAndComments();
                const ShaderLabSourceLocation location = CurrentLocation();
                if (m_offset >= m_source.size())
                    return { TokenKind::End, {}, location };

                const char c = m_source[m_offset];
                ++m_offset;
                switch (c)
                {
                case '{': return { TokenKind::LBrace, "{", location };
                case '}': return { TokenKind::RBrace, "}", location };
                case '(': return { TokenKind::LParen, "(", location };
                case ')': return { TokenKind::RParen, ")", location };
                case ',': return { TokenKind::Comma, ",", location };
                case '=': return { TokenKind::Equals, "=", location };
                case '"': return LexString(location);
                default:
                    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
                        return LexIdentifier(location, c);
                    if (std::isdigit(static_cast<unsigned char>(c)) ||
                        ((c == '+' || c == '-') && m_offset < m_source.size() &&
                            (std::isdigit(static_cast<unsigned char>(m_source[m_offset])) || m_source[m_offset] == '.')) ||
                        (c == '.' && m_offset < m_source.size() && std::isdigit(static_cast<unsigned char>(m_source[m_offset]))))
                        return LexNumber(location, c);
                    if (c == '+' || c == '-' || c == '.')
                        return LexIdentifier(location, c);
                    return { TokenKind::Identifier, std::string(1, c), location };
                }
            }

            Token LexString(const ShaderLabSourceLocation& location)
            {
                std::string text;
                bool closed = false;
                while (m_offset < m_source.size())
                {
                    const char c = m_source[m_offset++];
                    if (c == '"')
                    {
                        closed = true;
                        break;
                    }
                    text.push_back(c);
                }
                if (!closed)
                    AddDiagnostic(location, "unterminated string literal");
                return { TokenKind::String, std::move(text), location };
            }

            Token LexIdentifier(const ShaderLabSourceLocation& location, const char first)
            {
                std::string text(1, first);
                while (m_offset < m_source.size())
                {
                    const char c = m_source[m_offset];
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.')
                        break;
                    text.push_back(c);
                    ++m_offset;
                }
                return { TokenKind::Identifier, std::move(text), location };
            }

            Token LexNumber(const ShaderLabSourceLocation& location, const char first)
            {
                std::string text(1, first);
                while (m_offset < m_source.size())
                {
                    const char c = m_source[m_offset];
                    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != 'e' && c != 'E' && c != '+' && c != '-')
                        break;
                    text.push_back(c);
                    ++m_offset;
                }
                return { TokenKind::Number, std::move(text), location };
            }

            void SkipWhitespaceAndComments()
            {
                while (m_offset < m_source.size())
                {
                    const char c = m_source[m_offset];
                    if (std::isspace(static_cast<unsigned char>(c)))
                    {
                        ++m_offset;
                        continue;
                    }
                    if (c == '/' && m_offset + 1 < m_source.size() && m_source[m_offset + 1] == '/')
                    {
                        m_offset = FindLineEnd(m_offset);
                        continue;
                    }
                    break;
                }
            }

            ShaderLabSourceLocation CurrentLocation() const
            {
                return LocationAt(m_offset);
            }

            ShaderLabSourceLocation LocationAt(const size_t offset) const
            {
                ShaderLabSourceLocation location;
                location.file = m_filePath;
                location.byteOffset = static_cast<uint64_t>(offset);
                location.line = 1;
                location.column = 1;
                const size_t clamped = std::min(offset, m_source.size());
                for (size_t i = 0; i < clamped; ++i)
                {
                    if (m_source[i] == '\n')
                    {
                        ++location.line;
                        location.column = 1;
                    }
                    else
                    {
                        ++location.column;
                    }
                }
                return location;
            }

            static std::string_view Trim(std::string_view value)
            {
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                    value.remove_prefix(1);
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                    value.remove_suffix(1);
                return value;
            }

            std::string_view m_source;
            std::string m_filePath;
            size_t m_offset = 0;
            Token m_current;
            ShaderLabParseResult* m_result = nullptr;
        };

        std::string EscapeLineDirectiveFilename(std::string_view file)
        {
            std::string escaped;
            escaped.reserve(file.size());
            for (const char c : file)
            {
                if (c == '\\')
                {
                    escaped.push_back('/');
                }
                else if (c == '"')
                {
                    escaped.push_back('\\');
                    escaped.push_back('"');
                }
                else
                {
                    escaped.push_back(c);
                }
            }
            return escaped;
        }

        template<typename TPass>
        std::string BuildCompileSource(const TPass& pass)
        {
            std::ostringstream stream;
            stream << "#line " << pass.hlslLocation.line << " \""
                   << EscapeLineDirectiveFilename(pass.hlslLocation.file) << "\"\n";
            stream << pass.hlslSource;
            return stream.str();
        }
    }

    ShaderLabParseResult ParseShaderLabSource(std::string_view source, std::string filePath)
    {
        Parser parser(source, std::move(filePath));
        return parser.Parse();
    }

    std::string BuildShaderLabHlslForCompile(const ShaderLabPassDesc& pass)
    {
        return BuildCompileSource(pass);
    }

    std::string BuildShaderLabHlslForCompile(const ShaderLabPassRuntime& pass)
    {
        return BuildCompileSource(pass);
    }
}
