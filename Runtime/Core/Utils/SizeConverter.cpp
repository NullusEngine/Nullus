#include "Utils/SizeConverter.h"
using namespace NLS;
std::pair<float, Utils::SizeConverter::ESizeUnit> Utils::SizeConverter::ConvertToOptimalUnit(float p_value, ESizeUnit p_unit)
{
    if (p_value == 0.0f) return { 0.0f, ESizeUnit::BYTE };
    const float bytes = Convert(p_value, p_unit, ESizeUnit::BYTE);
    const int digits = static_cast<int>(trunc(log10(bytes)));
    const ESizeUnit targetUnit = static_cast<ESizeUnit>(fmin(3.0f * floor(digits / 3.0f), static_cast<float>(ESizeUnit::TERA_BYTE)));

    return { Convert(bytes, ESizeUnit::BYTE, targetUnit), targetUnit };
}

float Utils::SizeConverter::Convert(float p_value, ESizeUnit p_from, ESizeUnit p_to)
{
    const float fromValue = powf(1024.0f, static_cast<float>(p_from) / 3.0f);
    const float toValue = powf(1024.0f, static_cast<float>(p_to) / 3.0f);

    return p_value * (fromValue / toValue);
}

std::string Utils::SizeConverter::UnitToString(ESizeUnit p_unit)
{
    switch (p_unit)
    {
        case Utils::SizeConverter::ESizeUnit::BYTE:
            return "B";
        case Utils::SizeConverter::ESizeUnit::KILO_BYTE:
            return "KB";
        case Utils::SizeConverter::ESizeUnit::MEGA_BYTE:
            return "MB";
        case Utils::SizeConverter::ESizeUnit::GIGA_BYTE:
            return "GB";
        case Utils::SizeConverter::ESizeUnit::TERA_BYTE:
            return "TB";
    }

    return "?";
}
