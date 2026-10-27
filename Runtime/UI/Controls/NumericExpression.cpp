#include "UI/Controls/NumericExpression.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace NLS::UI::Controls
{
namespace
{
ExpressionValue Number(long double value)
{
    ExpressionValue result;
    result.number = value;
    if (!std::isfinite(value))
        result.error = "Expression produced a non-finite result";
    // Integers outside the exact binary64 range must come through the checked
    // integer path. long double is binary64 on MSVC.
    if (std::isfinite(value) && std::trunc(value) == value && std::abs(value) <= 9007199254740991.L)
    {
        result.exactInteger = true;
        result.negative = value < 0;
        result.magnitude = static_cast<uint64_t>(std::abs(value));
    }
    return result;
}
ExpressionValue Integer(uint64_t magnitude, bool negative)
{
    auto result = Number(negative ? -static_cast<long double>(magnitude) : static_cast<long double>(magnitude));
    result.magnitude = magnitude;
    result.negative = negative && magnitude != 0;
    result.exactInteger = true;
    return result;
}
bool Less(const ExpressionValue& a, const ExpressionValue& b)
{
    if (!a.exactInteger || !b.exactInteger)
        return a.number < b.number;
    if (a.negative != b.negative)
        return a.negative;
    return a.negative ? a.magnitude > b.magnitude : a.magnitude < b.magnitude;
}
ExpressionValue Binary(ExpressionValue a, ExpressionValue b, char op)
{
    if (!a)
        return a;
    if (!b)
        return b;
    if (op == '/' && b.number == 0)
        return {0, 0, false, false, "Division by zero"};
    auto result = Number(op == '+' ? a.number + b.number : op == '-' ? a.number - b.number :
                                                       op == '*'     ? a.number * b.number :
                                                                       a.number / b.number);
    if (!a.exactInteger || !b.exactInteger)
        return result;
    if (op == '-')
    {
        b.negative = !b.negative;
        op = '+';
    }
    if (op == '+')
    {
        if (a.negative != b.negative)
            return a.magnitude >= b.magnitude ? Integer(a.magnitude - b.magnitude, a.negative) : Integer(b.magnitude - a.magnitude, b.negative);
        if (a.magnitude <= UINT64_MAX - b.magnitude)
            return Integer(a.magnitude + b.magnitude, a.negative);
    }
    if (op == '*' && (b.magnitude == 0 || a.magnitude <= UINT64_MAX / b.magnitude))
        return Integer(a.magnitude * b.magnitude, a.negative != b.negative);
    if (op == '/' && b.magnitude && a.magnitude % b.magnitude == 0)
        return Integer(a.magnitude / b.magnitude, a.negative != b.negative);
    result.exactInteger = false;
    return result;
}
class Parser
{
public:
    explicit Parser(std::string_view text)
        : m_text(text)
    {
    }
    ExpressionValue Parse()
    {
        if (m_text.size() > 4096)
            return {0, 0, false, false, "Expression is too long"};
        auto value = Sum(0);
        Space();
        if (m_error.empty() && m_at != m_text.size())
            m_error = "Unexpected expression token";
        if (!m_error.empty())
            value.error = m_error;
        return value;
    }

private:
    void Space()
    {
        while (m_at < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_at])))
            ++m_at;
    }
    bool Take(char c)
    {
        Space();
        if (m_at < m_text.size() && m_text[m_at] == c)
        {
            ++m_at;
            return true;
        }
        return false;
    }
    ExpressionValue Sum(int depth)
    {
        auto a = Product(depth);
        for (;;)
        {
            if (Take('+'))
                a = Binary(a, Product(depth), '+');
            else if (Take('-'))
                a = Binary(a, Product(depth), '-');
            else
                return a;
        }
    }
    ExpressionValue Product(int depth)
    {
        auto a = Atom(depth);
        for (;;)
        {
            if (Take('*'))
                a = Binary(a, Atom(depth), '*');
            else if (Take('/'))
                a = Binary(a, Atom(depth), '/');
            else
                return a;
        }
    }
    ExpressionValue Atom(int depth)
    {
        if (depth > 64)
        {
            m_error = "Expression nesting limit exceeded";
            return {};
        }
        if (Take('+'))
            return Atom(depth + 1);
        if (Take('-'))
        {
            auto a = Atom(depth + 1);
            a.number = -a.number;
            a.negative = !a.negative;
            return a;
        }
        if (Take('('))
        {
            auto a = Sum(depth + 1);
            if (!Take(')'))
                m_error = "Expected closing parenthesis";
            return a;
        }
        Space();
        const size_t begin = m_at;
        if (m_at < m_text.size() && (std::isdigit(static_cast<unsigned char>(m_text[m_at])) || m_text[m_at] == '.'))
        {
            while (m_at < m_text.size() && (std::isdigit(static_cast<unsigned char>(m_text[m_at])) || m_text[m_at] == '.'))
                ++m_at;
            if (m_at < m_text.size() && (m_text[m_at] == 'e' || m_text[m_at] == 'E'))
            {
                ++m_at;
                if (m_at < m_text.size() && (m_text[m_at] == '+' || m_text[m_at] == '-'))
                    ++m_at;
                while (m_at < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_at])))
                    ++m_at;
            }
            const auto token = m_text.substr(begin, m_at - begin);
            if (token.find_first_of(".eE") == std::string_view::npos)
            {
                uint64_t integer = 0;
                auto parsed = std::from_chars(token.data(), token.data() + token.size(), integer);
                if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size())
                    return Integer(integer, false);
                m_error = "Integer literal overflow";
                return {};
            }
            const std::string literal(token);
            char* end = nullptr;
            const auto v = std::strtold(literal.c_str(), &end);
            if (end != literal.c_str() + literal.size())
                m_error = "Invalid number";
            auto number = Number(v);
            // A rounded decimal literal must not become an integer merely
            // because binary64 lost its fractional digits.
            std::string digits;
            int fraction = 0;
            bool decimal = false;
            const auto exponentAt = token.find_first_of("eE");
            for (char c : token.substr(0, exponentAt))
            {
                if (c == '.')
                    decimal = true;
                else
                {
                    digits += c;
                    if (decimal)
                        ++fraction;
                }
            }
            int exponent = 0;
            if (exponentAt != std::string_view::npos)
            {
                auto power = token.substr(exponentAt + 1);
                if (!power.empty() && power.front() == '+')
                    power.remove_prefix(1);
                const auto parsed = std::from_chars(power.data(), power.data() + power.size(), exponent);
                if (parsed.ec != std::errc{})
                    exponent = -100000;
            }
            while (fraction > 0 && !digits.empty() && digits.back() == '0')
            {
                digits.pop_back();
                --fraction;
            }
            if (digits.empty())
                digits = "0";
            uint64_t magnitude = 0;
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), magnitude);
            const int shift = exponent - fraction;
            bool exact = parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() && shift >= 0 && shift <= 19;
            for (int n = 0; exact && n < shift; ++n)
            {
                if (magnitude > UINT64_MAX / 10)
                    exact = false;
                else
                    magnitude *= 10;
            }
            if (exact)
                return Integer(magnitude, false);
            number.exactInteger = false;
            return number;
        }
        while (m_at < m_text.size() && (std::isalpha(static_cast<unsigned char>(m_text[m_at])) || std::isdigit(static_cast<unsigned char>(m_text[m_at])) || m_text[m_at] == '_'))
            ++m_at;
        const auto name = m_text.substr(begin, m_at - begin);
        if (name == "pi")
            return Number(std::acos(-1.L));
        if (name == "e")
            return Number(std::exp(1.L));
        if (name.empty() || !Take('('))
        {
            m_error = "Expected number, constant or function";
            return {};
        }
        std::vector<ExpressionValue> args;
        args.push_back(Sum(depth + 1));
        while (Take(','))
        {
            if (args.size() >= 3)
            {
                m_error = "Too many function arguments";
                return {};
            }
            args.push_back(Sum(depth + 1));
        }
        if (!Take(')'))
        {
            m_error = "Expected closing parenthesis";
            return {};
        }
        for (const auto& arg : args)
            if (!arg)
                return arg;
        const auto x = args[0].number;
        if (args.size() == 1)
        {
            if (name == "abs")
            {
                auto a = args[0];
                a.number = std::abs(a.number);
                a.negative = false;
                return a;
            }
            if (name == "sin")
                return Number(std::sin(x));
            if (name == "cos")
                return Number(std::cos(x));
            if (name == "tan")
                return Number(std::tan(x));
            if (name == "sqrt")
                return Number(std::sqrt(x));
            if (name == "floor")
                return Number(std::floor(x));
            if (name == "ceil")
                return Number(std::ceil(x));
            if (name == "round")
                return Number(std::round(x));
            if (name == "log")
                return Number(std::log(x));
            if (name == "exp")
                return Number(std::exp(x));
        }
        if (args.size() == 2)
        {
            if (name == "min")
                return Less(args[0], args[1]) ? args[0] : args[1];
            if (name == "max")
                return Less(args[0], args[1]) ? args[1] : args[0];
            if (name == "pow")
                return Number(std::pow(x, args[1].number));
        }
        if (args.size() == 3 && name == "clamp" && !Less(args[2], args[1]))
            return Less(args[0], args[1]) ? args[1] : Less(args[2], args[0]) ? args[2] :
                                                                               args[0];
        m_error = "Unknown function or invalid argument count";
        return {};
    }
    std::string_view m_text;
    size_t m_at = 0;
    std::string m_error;
};
} // namespace
ExpressionValue EvaluateExpression(std::string_view text)
{
    return Parser(text).Parse();
}
} // namespace NLS::UI::Controls
