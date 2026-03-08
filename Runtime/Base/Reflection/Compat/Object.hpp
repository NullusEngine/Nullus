#pragma once

#include "config.hpp"
#include "Reflection/Meta.h"
#include "Reflection/Argument.h"
#include "Reflection/Variant.h"
#include <type_traits>
#include <utility>
#include <vector>

namespace NLS::UDRefl {

using Type = NLS::meta::Type;

class TempArgsView {
public:
    TempArgsView() = default;

    template<typename... Args>
    explicit TempArgsView(Args&&... args) {
        (m_args.emplace_back(std::forward<Args>(args)), ...);
    }

    NLS::meta::ArgumentList& Args() { return m_args; }
    const NLS::meta::ArgumentList& Args() const { return m_args; }

private:
    NLS::meta::ArgumentList m_args;
};

class ObjectView {
public:
    ObjectView() = default;
    explicit ObjectView(const NLS::meta::Variant& v) : m_var(v) {}
    explicit ObjectView(NLS::meta::Variant&& v) : m_var(std::move(v)) {}

    template<typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, NLS::meta::Variant>>>
    explicit ObjectView(T& obj) : m_var(obj, NLS::meta::variant_policy::NoCopy{}) {}

    bool Valid() const { return m_var.IsValid(); }
    explicit operator bool() const { return Valid(); }

    Type GetType() const { return m_var.GetType(); }

    template<typename T>
    T& As() const { return m_var.GetValue<T>(); }

    template<typename T>
    T* AsPtr() const {
        if constexpr (std::is_pointer_v<T>) {
            return m_var.GetValue<T>();
        } else if (GetType().IsPointer()) {
            return m_var.GetValue<T*>();
        } else {
            return &m_var.GetValue<T>();
        }
    }

    ObjectView StaticCast_DerivedToBase(Type type) const;
    ObjectView StaticCast_BaseToDerived(Type type) const;
    ObjectView StaticCast(Type type) const;

    ObjectView Var(std::string_view field_name) const;

    ObjectView Invoke(const std::string& methodName) const;
    ObjectView Invoke(const std::string& methodName, const TempArgsView& args) const;

    template<typename Ret>
    Ret Invoke(const std::string& methodName) const {
        auto v = Invoke(methodName);
        if constexpr (std::is_pointer_v<Ret>) {
            using Elem = std::remove_pointer_t<Ret>;
            return v.AsPtr<Elem>();
        } else {
            return v.As<Ret>();
        }
    }

    bool operator==(const ObjectView& rhs) const {
        return GetType() == rhs.GetType() && m_var.ToString() == rhs.m_var.ToString();
    }
    bool operator!=(const ObjectView& rhs) const { return !(*this == rhs); }

    NLS::meta::Variant& Variant() { return m_var; }
    const NLS::meta::Variant& Variant() const { return m_var; }

protected:
    NLS::meta::Variant m_var;
};

class SharedObject : public ObjectView {
public:
    SharedObject() = default;
    explicit SharedObject(const NLS::meta::Variant& v) : ObjectView(v) {}
    explicit SharedObject(NLS::meta::Variant&& v) : ObjectView(std::move(v)) {}

    SharedObject& operator*() { return *this; }
    const SharedObject& operator*() const { return *this; }
};

} // namespace NLS::UDRefl
