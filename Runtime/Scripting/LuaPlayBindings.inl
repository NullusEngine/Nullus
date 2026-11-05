// Included beside the native Lua object adapter; both languages call the same checked dispatcher.
const char* LuaPlayType(NLS::Object* object)
{
    if(dynamic_cast<NLS::Engine::Components::RigidbodyComponent*>(object))return "Rigidbody";
    if(dynamic_cast<NLS::Engine::Components::PhysicsColliderComponent*>(object))return "Collider";
    if(dynamic_cast<NLS::Engine::Components::AudioSourceComponent*>(object))return "AudioSource";
    if(dynamic_cast<NLS::Engine::Components::CameraComponent*>(object))return "Camera";
    return nullptr;
}
bool LuaPlayPropertyKnown(const char* type,const char* key,bool write)
{
    const std::string_view name(key),kind(type);
    if(kind=="Rigidbody")return name=="mass"||name=="isKinematic"||name=="useGravity"||name=="velocity";
    if(kind=="Collider")return name=="isTrigger"||name=="radius"||name=="size"||name=="shape";
    if(kind=="AudioSource")return name=="volume"||name=="loop"||name=="playOnAwake"||name=="clip"||(!write&&name=="isPlaying");
    return kind=="Camera"&&(name=="targetDisplay"||name=="depth"||name=="rect"||name=="clearFlags");
}
uint32_t ReadLuaPlayArguments(lua_State* state,int index,ScriptAbiValue* arguments)
{
    if(lua_gettop(state)<index)return 0;
    if(auto* object=TestNativeObject(state,index)) {arguments[0].objectValue=object->handle;return 1;}
    if(lua_isnil(state,index)){arguments[0].objectValue=0;return 1;}
    if(lua_istable(state,index))
    {
        const char* keys[]={"x","y","z","w"};
        uint32_t count=0;
        for(uint32_t i=0;i<4;++i)
        {
            lua_getfield(state,index,keys[i]);
            if(lua_isnil(state,-1)&&i>=2){lua_pop(state,1);lua_getfield(state,index,i==2?"width":"height");}
            if(lua_isnumber(state,-1)){arguments[i].floatingValue=lua_tonumber(state,-1);++count;}
            lua_pop(state,1);
        }
        return count;
    }
    arguments[0].floatingValue=lua_isboolean(state,index)?lua_toboolean(state,index):luaL_checknumber(state,index);
    return 1;
}
ScriptAbiValue CallLuaPlayMember(lua_State* state,const std::string& member,int argumentIndex)
{
    auto* self=TestNativeObject(state,1);
    ScriptAbiValue arguments[4]{},result{};
    const auto count=argumentIndex?ReadLuaPlayArguments(state,argumentIndex,arguments):0;
    const auto status=InvokePlayMember(self?self->handle:0,MakeStableScriptId("Nullus.Play::"+member),arguments,count,&result);
    if(status.code!=ScriptStatusCode::Ok)luaL_error(state,"%s",status.message?status.message:"Play member failed.");
    return result;
}
int LuaPlayMethod(lua_State* state)
{
    (void)CallLuaPlayMember(state,lua_tostring(state,lua_upvalueindex(1)),2);return 0;
}
bool TryGetLuaPlayProperty(lua_State* state,NLS::Object* object,const char* key)
{
    const char* type=LuaPlayType(object);if(!type)return false;
    const bool method=(std::strcmp(type,"Rigidbody")==0&&std::strcmp(key,"AddForce")==0)||
        (std::strcmp(type,"AudioSource")==0&&(std::strcmp(key,"Play")==0||std::strcmp(key,"Pause")==0||std::strcmp(key,"Stop")==0));
    const std::string member=std::string(type)+"."+key;
    if(method){lua_pushstring(state,member.c_str());lua_pushcclosure(state,LuaPlayMethod,1);return true;}
    if(!LuaPlayPropertyKnown(type,key,false))return false;
    const auto result=CallLuaPlayMember(state,member+".get",0);
    if(result.kind==uint32_t(ScriptValueKind::ObjectReference))
    {if(result.objectValue)PushNativeObject(state,{result.objectValue});else lua_pushnil(state);}
    else if(result.byteSize>=3*sizeof(float))
    {
        const auto* data=reinterpret_cast<const float*>(result.bytes);
        if(result.byteSize==3*sizeof(float))PushVector3(state,{data[0],data[1],data[2]});
        else {lua_createtable(state,0,4);const char* names[]={"x","y","width","height"};for(int i=0;i<4;++i){lua_pushnumber(state,data[i]);lua_setfield(state,-2,names[i]);}}
    }
    else if(std::string_view(key).starts_with("is")||std::strcmp(key,"useGravity")==0||std::strcmp(key,"loop")==0||std::strcmp(key,"playOnAwake")==0)lua_pushboolean(state,result.floatingValue!=0);
    else lua_pushnumber(state,result.floatingValue);
    return true;
}
bool TrySetLuaPlayProperty(lua_State* state,NLS::Object* object,const char* key)
{
    const char* type=LuaPlayType(object);if(!type||!LuaPlayPropertyKnown(type,key,true))return false;
    (void)CallLuaPlayMember(state,std::string(type)+"."+key+".set",3);return true;
}
