#include "Rendering/Resources/MaterialSerialization.h"

#include <cctype>
#include <sstream>
#include <vector>
#include <cmath>
#include <Json/json.hpp>

namespace NLS::Render::Resources
{
    std::string SerializeMaterialSampler(const RHI::SamplerDesc& s)
    {
        return nlohmann::json{{"wrapU",static_cast<int>(s.wrapU)},{"wrapV",static_cast<int>(s.wrapV)},
            {"wrapW",static_cast<int>(s.wrapW)},{"min",static_cast<int>(s.minFilter)},{"mag",static_cast<int>(s.magFilter)},
            {"mip",static_cast<int>(s.mipFilter)},{"anisotropy",s.maxAnisotropy},{"minLod",s.minLod},{"maxLod",s.maxLod},
            {"bias",s.mipLodBias},{"compare",s.compareEnabled},{"compareFunc",static_cast<int>(s.compareFunc)},{"border",s.borderColor}}.dump();
    }
    std::optional<RHI::SamplerDesc> DeserializeMaterialSampler(std::string_view text)
    {
        try
        {
            const auto j=nlohmann::json::parse(text);RHI::SamplerDesc s;
            const auto u=j.at("wrapU").get<int>(),v=j.at("wrapV").get<int>(),w=j.at("wrapW").get<int>();
            const auto min=j.at("min").get<int>(),mag=j.at("mag").get<int>(),mip=j.value("mip",1),compare=j.value("compareFunc",0);
            if(u<0 || u>3 || v<0 || v>3 || w<0 || w>3 || min<0 || min>1 || mag<0 || mag>1 || mip<0 || mip>1 || compare<0 || compare>7)return std::nullopt;
            s.wrapU=static_cast<RHI::TextureWrap>(u);s.wrapV=static_cast<RHI::TextureWrap>(v);s.wrapW=static_cast<RHI::TextureWrap>(w);
            s.minFilter=static_cast<RHI::TextureFilter>(min);s.magFilter=static_cast<RHI::TextureFilter>(mag);s.mipFilter=static_cast<RHI::TextureMipFilter>(mip);
            s.maxAnisotropy=j.value("anisotropy",1u);s.minLod=j.value("minLod",0.f);s.maxLod=j.value("maxLod",RHI::SamplerDesc::UnboundedMaxLod);s.mipLodBias=j.value("bias",0.f);
            s.compareEnabled=j.value("compare",false);s.compareFunc=static_cast<Settings::EComparaisonAlgorithm>(compare);
            if(j.contains("border"))s.borderColor=j.at("border").get<std::array<float,4>>();
            if(!s.maxAnisotropy || s.maxAnisotropy>16 || !std::isfinite(s.minLod) || !std::isfinite(s.maxLod) ||
                !std::isfinite(s.mipLodBias) || s.minLod>s.maxLod)return std::nullopt;
            for(float value:s.borderColor)if(!std::isfinite(value))return std::nullopt;
            return s;
        }
        catch(const std::exception&){return std::nullopt;}
    }
    std::vector<SerializedMaterialAssetReference> CollectSerializedMaterialAssetReferences(std::string_view payload)
    {
        std::vector<SerializedMaterialAssetReference> references;
        std::istringstream stream{std::string(payload)}; std::string line;
        while (std::getline(stream,line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("shader=")) references.push_back({"shader",UnescapeMaterialField(line.substr(7)),true});
            else if (line.starts_with("property "))
            {
                std::istringstream property(line); std::string keyword,name,type,value;
                property >> keyword >> name >> type;
                std::getline(property >> std::ws,value);
                if ((type == "Texture2D" || type == "TextureCube") && !value.empty())
                    references.push_back({std::move(name),UnescapeMaterialField(value),false});
            }
        }
        return references;
    }
    namespace
    {
        bool IsMaterialFieldSafeCharacter(const unsigned char character)
        {
            return std::isalnum(character) != 0 ||
                character == '_' ||
                character == '-' ||
                character == '.' ||
                character == '/' ||
                character == ':' ||
                character == '?';
        }

        char HexDigit(const unsigned int value)
        {
            return static_cast<char>(value < 10u ? ('0' + value) : ('A' + value - 10u));
        }

        int HexValue(const char character)
        {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            return -1;
        }

        std::vector<std::string> SplitWhitespace(const std::string_view value)
        {
            std::vector<std::string> tokens;
            std::istringstream stream{std::string(value)};
            std::string token;
            while (stream >> token)
                tokens.push_back(std::move(token));
            return tokens;
        }
    }

    std::string EscapeMaterialField(const std::string_view value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const unsigned char character : value)
        {
            if (IsMaterialFieldSafeCharacter(character))
            {
                escaped.push_back(static_cast<char>(character));
                continue;
            }

            escaped.push_back('%');
            escaped.push_back(HexDigit((character >> 4u) & 0xFu));
            escaped.push_back(HexDigit(character & 0xFu));
        }
        return escaped;
    }

    std::string UnescapeMaterialField(const std::string_view value)
    {
        std::string unescaped;
        unescaped.reserve(value.size());
        for (size_t index = 0u; index < value.size(); ++index)
        {
            if (value[index] != '%' || index + 2u >= value.size())
            {
                unescaped.push_back(value[index]);
                continue;
            }

            const int high = HexValue(value[index + 1u]);
            const int low = HexValue(value[index + 2u]);
            if (high < 0 || low < 0)
            {
                unescaped.push_back(value[index]);
                continue;
            }

            unescaped.push_back(static_cast<char>((high << 4) | low));
            index += 2u;
        }
        return unescaped;
    }

    std::map<std::string, std::string> ParseMaterialKeyValueTail(const std::string_view tail)
    {
        std::map<std::string, std::string> values;
        for (const auto& token : SplitWhitespace(tail))
        {
            const auto equals = token.find('=');
            if (equals == std::string::npos || equals == 0u)
                continue;
            values[token.substr(0u, equals)] = UnescapeMaterialField(token.substr(equals + 1u));
        }
        return values;
    }
}
