#pragma once

#include <Json/json.hpp>

#include <string>

namespace NLS
{
    using json = nlohmann::json;

    class Json
    {
    public:
        using native_type = nlohmann::json;
        using object = native_type::object_t;
        using array = native_type::array_t;

        Json() = default;
        Json(std::nullptr_t)
            : m_value(nullptr)
        {
        }
        Json(const native_type& value)
            : m_value(value)
        {
        }
        Json(native_type&& value)
            : m_value(std::move(value))
        {
        }
        Json(const object& value)
            : m_value(value)
        {
        }
        Json(object&& value)
            : m_value(std::move(value))
        {
        }
        Json(const array& value)
            : m_value(value)
        {
        }
        Json(array&& value)
            : m_value(std::move(value))
        {
        }
        Json(const char* value)
            : m_value(value)
        {
        }
        Json(const std::string& value)
            : m_value(value)
        {
        }
        Json(std::string&& value)
            : m_value(std::move(value))
        {
        }
        Json(bool value)
            : m_value(value)
        {
        }
        Json(int value)
            : m_value(value)
        {
        }
        Json(unsigned int value)
            : m_value(value)
        {
        }
        Json(int64_t value)
            : m_value(value)
        {
        }
        Json(uint64_t value)
            : m_value(value)
        {
        }
        Json(float value)
            : m_value(value)
        {
        }
        Json(double value)
            : m_value(value)
        {
        }

        static Json parse(const std::string& input, std::string& error)
        {
            error.clear();
            auto parsed = native_type::parse(input, nullptr, false);
            if (parsed.is_discarded())
            {
                error = "Invalid JSON";
                return {};
            }
            return Json(std::move(parsed));
        }

        template <typename TParseMode>
        static Json parse(const std::string& input, std::string& error, TParseMode)
        {
            return parse(input, error);
        }

        bool is_null() const { return m_value.is_null(); }
        bool is_bool() const { return m_value.is_boolean(); }
        bool is_number() const { return m_value.is_number(); }
        bool is_string() const { return m_value.is_string(); }
        bool is_array() const { return m_value.is_array(); }
        bool is_object() const { return m_value.is_object(); }

        bool bool_value() const
        {
            return m_value.is_boolean() ? m_value.get<bool>() : false;
        }

        int int_value() const
        {
            return m_value.is_number_integer()
                ? m_value.get<int>()
                : static_cast<int>(number_value());
        }

        double number_value() const
        {
            return m_value.is_number() ? m_value.get<double>() : 0.0;
        }

        std::string string_value() const
        {
            return m_value.is_string() ? m_value.get<std::string>() : std::string {};
        }

        const array& array_items() const
        {
            static const array empty;
            return m_value.is_array() ? m_value.get_ref<const array&>() : empty;
        }

        const object& object_items() const
        {
            static const object empty;
            return m_value.is_object() ? m_value.get_ref<const object&>() : empty;
        }

        std::string dump() const
        {
            return m_value.dump();
        }

        Json operator[](const std::string& key) const
        {
            if (!m_value.is_object())
                return {};
            const auto found = m_value.find(key);
            return found != m_value.end() ? Json(*found) : Json {};
        }

        Json operator[](const char* key) const
        {
            return (*this)[std::string(key)];
        }

        Json operator[](std::size_t index) const
        {
            if (!m_value.is_array() || index >= m_value.size())
                return {};
            return Json(m_value[index]);
        }

        const native_type& native() const
        {
            return m_value;
        }

        native_type& native()
        {
            return m_value;
        }

        operator const native_type&() const
        {
            return m_value;
        }

    private:
        native_type m_value;
    };
}
