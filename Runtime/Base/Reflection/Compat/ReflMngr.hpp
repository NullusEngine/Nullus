#pragma once

#include "Object.hpp"
#include "Reflection/ReflectionDatabase.h"
#include "Reflection/TypeInfo.h"
#include "Reflection/TypeData.h"
#include "Reflection/Method.h"
#include <typeinfo>
#include <utility>

namespace NLS::UDRefl {

class ReflMngr {
public:
    static ReflMngr& Instance() noexcept {
        static ReflMngr s;
        return s;
    }

    template<typename T>
    void RegisterType() {
        auto& db = NLS::meta::ReflectionDatabase::Instance();
        const std::string name = typeid(T).name();
        NLS::meta::TypeID id = NLS::meta::InvalidTypeID;
        auto it = db.ids.find(name);
        if (it == db.ids.end()) {
            id = db.AllocateType(name);
            if (id != NLS::meta::InvalidTypeID) {
                auto& data = db.types[id];
                NLS::meta::TypeInfo<T>::Register(id, data, true);
            }
        } else {
            id = it->second;
        }
        NLS::meta::TypeIDs<T>::ID = id;
    }

    template<typename Derived, typename Base>
    void AddBases() {
        RegisterType<Derived>();
        RegisterType<Base>();
        auto& db = NLS::meta::ReflectionDatabase::Instance();
        auto did = NLS::meta::TypeIDs<Derived>::ID;
        db.types[did].LoadBaseClasses(db, did, { NLS::meta::Type(typeidof(Base)) });
    }

    template<auto MemberPtr>
    void AddField(const std::string& name) {
        using Traits = MemberPointerTraits<decltype(MemberPtr)>;
        using C = typename Traits::ClassType;
        using F = typename Traits::FieldType;
        RegisterType<C>();
        auto& db = NLS::meta::ReflectionDatabase::Instance();
        auto id = NLS::meta::TypeIDs<C>::ID;
        db.types[id].template AddField<C, F>(name, MemberPtr, MemberPtr, {});
    }

    template<auto MethodPtr>
    void AddMethod(const std::string& name) {
        using C = typename MethodPointerTraits<decltype(MethodPtr)>::ClassType;
        RegisterType<C>();
        auto& db = NLS::meta::ReflectionDatabase::Instance();
        auto id = NLS::meta::TypeIDs<C>::ID;
        db.types[id].AddMethod(name, MethodPtr, {});
    }

    bool IsDerivedFrom(Type d, Type b) const { return d.DerivesFrom(b); }

    SharedObject MakeShared(Type type, const TempArgsView& args = {}) const {
        auto ctor = type.GetDynamicConstructor();
        auto v = ctor.IsValid() ? ctor.InvokeVariadic(args.Args()) : NLS::meta::Variant{};
        return SharedObject{std::move(v)};
    }

    ObjectView New(Type type, const TempArgsView& args = {}) const {
        auto ctor = type.GetDynamicConstructor();
        auto v = ctor.IsValid() ? ctor.InvokeVariadic(args.Args()) : NLS::meta::Variant{};
        return ObjectView{std::move(v)};
    }

private:
    template<typename T> struct MemberPointerTraits;
    template<typename C, typename F> struct MemberPointerTraits<F C::*> { using ClassType = C; using FieldType = F; };

    template<typename T> struct MethodPointerTraits;
    template<typename C, typename R, typename... A>
    struct MethodPointerTraits<R(C::*)(A...)> { using ClassType = C; };
    template<typename C, typename R, typename... A>
    struct MethodPointerTraits<R(C::*)(A...) const> { using ClassType = C; };
};

inline ObjectView ObjectView::StaticCast_DerivedToBase(Type type) const { (void)type; return *this; }
inline ObjectView ObjectView::StaticCast_BaseToDerived(Type type) const { (void)type; return *this; }
inline ObjectView ObjectView::StaticCast(Type type) const { (void)type; return *this; }

inline ObjectView ObjectView::Var(std::string_view field_name) const {
    auto field = GetType().GetField(std::string(field_name));
    if (!field.IsValid()) return {};
    auto value = field.GetValueReference(const_cast<NLS::meta::Variant&>(m_var));
    return ObjectView{value};
}

inline ObjectView ObjectView::Invoke(const std::string& methodName) const {
    TempArgsView empty;
    return Invoke(methodName, empty);
}

inline ObjectView ObjectView::Invoke(const std::string& methodName, const TempArgsView& args) const {
    auto m = GetType().GetMethod(methodName);
    if (!m.IsValid()) return {};
    auto self = m_var;
    auto argv = args.Args();
    auto ret = m.Invoke(self, argv);
    return ObjectView{ret};
}

inline static ReflMngr& Mngr = ReflMngr::Instance();

} // namespace NLS::UDRefl

namespace NLS {
using Type = NLS::meta::Type;
template<typename T>
inline Type Type_of = NLS::meta::Type(typeidof(T));
}

namespace NLS::UDRefl {
using NLS::Type;
using NLS::Type_of;
}

