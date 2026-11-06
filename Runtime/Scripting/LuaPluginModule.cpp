#include "LuaScriptBackend.h"
#include <Debug/Logger.h>
#include <fstream>
#include <stdexcept>
#if NLS_HAS_LUA_VM
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#endif
namespace NLS::Scripting
{
#if NLS_HAS_LUA_VM
namespace
{
using namespace Core::Modules;
struct LuaPluginState : std::enable_shared_from_this<LuaPluginState>
{
    std::shared_ptr<lua_State> vm;
    ModuleContext* startup = nullptr;
    std::weak_ptr<ModuleScope> scope;
    std::vector<RegistrationToken> registrations;
    std::vector<int> functions,coroutines;
    int environment = LUA_NOREF, module = LUA_NOREF;
    bool accepting = true, stopFailed = false, cleaned = false;
    std::string diagnostic;
    explicit LuaPluginState(std::shared_ptr<lua_State> value) : vm(std::move(value)) {}
    static void Budget(lua_State* state,lua_Debug*) { luaL_error(state,"plugin instruction budget exceeded"); }
    struct Frame
    {
        lua_State* state; int top; lua_Hook hook; int mask,count;
        explicit Frame(lua_State* s) : state(s),top(lua_gettop(s)),hook(lua_gethook(s)),mask(lua_gethookmask(s)),count(lua_gethookcount(s))
        { lua_sethook(s,Budget,LUA_MASKCOUNT,1000000); }
        ~Frame() { lua_settop(state,top); lua_sethook(state,hook,mask,count); }
    };
    static LuaPluginState* Self(lua_State* state) { return static_cast<LuaPluginState*>(lua_touserdata(state,lua_upvalueindex(1))); }
    static std::string Error(lua_State* state,const char* context)
    { const auto* message=lua_tostring(state,-1); return std::string(context) + (message ? message : "non-string Lua error"); }
    static int Register(lua_State* state)
    {
        auto* self = Self(state);
        if (!self->accepting || !self->startup || lua_type(state,1)!=LUA_TSTRING || lua_type(state,2)!=LUA_TSTRING || !lua_isfunction(state,3) ||
            (!lua_isnoneornil(state,4) && !lua_isinteger(state,4)))
        { lua_pushnil(state); lua_pushliteral(state,"registration requires active Startup and (id, kind, function)"); return 2; }
        try
        {
            ExtensionDescriptor descriptor{lua_tostring(state,1),lua_tostring(state,2),static_cast<int32_t>(lua_isnoneornil(state,4) ? 0 : lua_tointeger(state,4))};
            lua_pushvalue(state,3); const auto function = luaL_ref(state,LUA_REGISTRYINDEX); self->functions.push_back(function);
            auto token = self->startup->extensions.Register(self->startup->scope,std::move(descriptor),[owner=self->shared_from_this(),function](ExtensionCall& call) {
                auto* lua = owner->vm.get(); Frame frame(lua);
                lua_rawgeti(lua,LUA_REGISTRYINDEX,function); lua_pushinteger(lua,call.event); lua_pushnumber(lua,call.deltaSeconds); lua_pushinteger(lua,static_cast<lua_Integer>(call.value));
                if (lua_pcall(lua,3,1,0) != LUA_OK) throw std::runtime_error(Error(lua,"Lua plugin callback: "));
                if (lua_isinteger(lua,-1)) call.value = static_cast<uint64_t>(lua_tointeger(lua,-1));
            });
            if (!token) throw std::runtime_error("duplicate/invalid Lua extension registration");
            self->registrations.push_back(std::move(token)); lua_pushboolean(state,1); return 1;
        }
        catch (const std::exception& error) { lua_pushnil(state); lua_pushstring(state,error.what()); return 2; }
    }
    static int StartCoroutine(lua_State* state)
    {
        auto* self = Self(state);
        if (!self->accepting || !lua_isfunction(state,1)) { lua_pushnil(state); return 1; }
        auto* thread = lua_newthread(state); const auto reference = luaL_ref(state,LUA_REGISTRYINDEX);
        lua_pushvalue(state,1); lua_xmove(state,thread,1); self->coroutines.push_back(reference); lua_pushboolean(state,1); return 1;
    }
    static int Log(lua_State* state)
    {
        auto* self = Self(state); auto owner = self->scope.lock();
        if (!self->accepting || !owner) { lua_pushboolean(state,0); return 1; }
        try { NLS_LOG_INFO("[Lua plugin " + owner->Id() + "] " + (lua_type(state,1)==LUA_TSTRING ? lua_tostring(state,1) : "")); }
        catch (...) { lua_pushboolean(state,0); return 1; }
        lua_pushboolean(state,1); return 1;
    }
    void Pump()
    {
        auto* state = vm.get(); Frame frame(state);
        for (auto it = coroutines.begin(); it != coroutines.end();)
        {
            lua_rawgeti(state,LUA_REGISTRYINDEX,*it); auto* thread = lua_tothread(state,-1); lua_pop(state,1);
            int results = 0,status;
            const auto hook = lua_gethook(thread); const int mask = lua_gethookmask(thread), count = lua_gethookcount(thread);
            lua_sethook(thread,Budget,LUA_MASKCOUNT,1000000); status = lua_resume(thread,state,0,&results);
            lua_sethook(thread,hook,mask,count); lua_settop(thread,0);
            if (status == LUA_YIELD) { ++it; continue; }
            if (status != LUA_OK) { diagnostic = "Lua plugin coroutine failed"; NLS_LOG_ERROR(diagnostic); }
            lua_resetthread(thread); luaL_unref(state,LUA_REGISTRYINDEX,*it); it = coroutines.erase(it);
        }
    }
    void Stop() noexcept
    {
        accepting = false; startup = nullptr;
        auto* state = vm.get(); Frame frame(state);
        for (auto reference : coroutines)
        {
            lua_rawgeti(state,LUA_REGISTRYINDEX,reference); auto* thread = lua_tothread(state,-1); lua_pop(state,1);
            // A failing __close handler is a cleanup failure, never a successful unload.
            if (lua_resetthread(thread) != LUA_OK) { stopFailed = true; diagnostic = "Lua coroutine close failed; restart required"; }
        }
        if (stopFailed) NLS_LOG_ERROR(diagnostic);
    }
    void Clean()
    {
        if (cleaned) return;
        auto* state = vm.get(); Frame frame(state);
        if (module != LUA_NOREF)
        {
            lua_rawgeti(state,LUA_REGISTRYINDEX,module); lua_getfield(state,-1,"shutdown");
            if (lua_isfunction(state,-1) && lua_pcall(state,0,0,0) != LUA_OK)
                throw std::runtime_error(Error(state,"Lua plugin shutdown failed: "));
        }
        registrations.clear();
        for (auto ref : coroutines) luaL_unref(state,LUA_REGISTRYINDEX,ref); coroutines.clear();
        for (auto ref : functions) luaL_unref(state,LUA_REGISTRYINDEX,ref); functions.clear();
        if (module != LUA_NOREF) luaL_unref(state,LUA_REGISTRYINDEX,module);
        if (environment != LUA_NOREF) luaL_unref(state,LUA_REGISTRYINDEX,environment);
        module = environment = LUA_NOREF; cleaned = true;
    }
};
class LuaPluginModule final : public IModule
{
    std::shared_ptr<LuaPluginState> m_state;
    std::filesystem::path m_path;
public:
    LuaPluginModule(std::shared_ptr<lua_State> state,std::filesystem::path path) : m_state(std::make_shared<LuaPluginState>(std::move(state))),m_path(std::move(path)) {}
    bool Startup(ModuleContext& context) override
    {
        auto self = m_state; auto* state = self->vm.get(); LuaPluginState::Frame frame(state);
        self->startup = &context; self->scope = context.scope;
        context.scope->TrackCleanup([self] { self->Clean(); });
        lua_newtable(state); const int environment = lua_gettop(state);
        // Copy safe scalar functions and library tables. The environment cannot
        // mutate shared game globals or obtain untracked coroutine/Registry refs.
        for (const char* name : {"assert","error","ipairs","pairs","next","pcall","select","tonumber","tostring","type","xpcall"})
        { lua_getglobal(state,name); lua_setfield(state,environment,name); }
        for (const char* name : {"math","string","table","utf8"})
        {
            lua_newtable(state); const int copy = lua_gettop(state); lua_getglobal(state,name); lua_pushnil(state);
            while (lua_next(state,-2)) { lua_pushvalue(state,-2); lua_pushvalue(state,-2); lua_settable(state,copy); lua_pop(state,1); }
            lua_pop(state,1); lua_setfield(state,environment,name);
        }
        lua_newtable(state); lua_getglobal(state,"coroutine");
        for (const char* name : {"yield","isyieldable","running"})
        { lua_getfield(state,-1,name); lua_setfield(state,-3,name); }
        lua_pop(state,1); lua_setfield(state,environment,"coroutine");
        lua_newtable(state);
        // The entry points are taken by address: a bare member function would
        // deduce a function type, which cannot be a pair member.
        for (auto entry : {std::pair{"register_extension",&LuaPluginState::Register},std::pair{"start_coroutine",&LuaPluginState::StartCoroutine},std::pair{"log",&LuaPluginState::Log}})
        { lua_pushlightuserdata(state,self.get()); lua_pushcclosure(state,entry.second,1); lua_setfield(state,-2,entry.first); }
        lua_setfield(state,environment,"host");
        lua_pushvalue(state,environment); self->environment = luaL_ref(state,LUA_REGISTRYINDEX);
        if (std::filesystem::file_size(m_path)>8*1024*1024) throw std::runtime_error("Lua plugin source limit exceeded");
        std::ifstream input(m_path,std::ios::binary); if (!input) throw std::runtime_error("cannot read Lua plugin");
        const std::string source((std::istreambuf_iterator<char>(input)),{});
        if (source.size() > 8*1024*1024) throw std::runtime_error("Lua plugin source limit exceeded");
        if (luaL_loadbufferx(state,source.data(),source.size(),m_path.generic_string().c_str(),"t") != LUA_OK)
            throw std::runtime_error(LuaPluginState::Error(state,"Lua plugin compilation: "));
        lua_pushvalue(state,environment); lua_setupvalue(state,-2,1);
        if (lua_pcall(state,0,1,0) != LUA_OK) throw std::runtime_error(LuaPluginState::Error(state,"Lua plugin entry: "));
        if (!lua_istable(state,-1)) throw std::runtime_error("Lua plugin must return a module table");
        lua_pushvalue(state,-1); self->module = luaL_ref(state,LUA_REGISTRYINDEX);
        lua_getfield(state,-1,"startup");
        if (lua_isfunction(state,-1))
        { lua_getfield(state,environment,"host"); if (lua_pcall(state,1,0,0) != LUA_OK) throw std::runtime_error(LuaPluginState::Error(state,"Lua plugin startup: ")); }
        self->startup = nullptr;
        self->registrations.push_back(context.extensions.Register(context.scope,{context.scope->Id()+".coroutines","runtime.play",0},[self](ExtensionCall& call) { if (call.event == 2) self->Pump(); }));
        return true;
    }
    void RequestStop() noexcept override { m_state->Stop(); }
    void AbortStartup() noexcept override { m_state->Stop(); }
    bool IsStopped() const noexcept override { return !m_state->stopFailed && !m_state->accepting; }
    std::string UnloadDiagnostic() const override { return m_state->diagnostic; }
    void Shutdown() noexcept override {}
};
}
#endif
std::unique_ptr<Core::Modules::IModule> LuaScriptBackend::CreatePluginModule(const std::filesystem::path& path)
{
#if NLS_HAS_LUA_VM
    return m_initialized && m_pluginVm ? std::make_unique<LuaPluginModule>(m_pluginVm,path) : nullptr;
#else
    return {};
#endif
}
}
