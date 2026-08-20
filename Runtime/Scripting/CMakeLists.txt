set(NLS_SCRIPTING_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptingDef.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptTypes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptApiManifest.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptApiManifest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/IScriptBackend.h
    ${CMAKE_CURRENT_SOURCE_DIR}/IScriptBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptRuntime.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptRuntime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptEngineApi.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptEngineApi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptRuntimeSetup.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptRuntimeSetup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptComponent.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptComponent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/FakeScriptBackend.h
    ${CMAKE_CURRENT_SOURCE_DIR}/FakeScriptBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaScriptBackend.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaScriptBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CoreClrScriptBackend.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CoreClrScriptBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptErrorConsole.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptDebug.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptDebug.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptFieldSerialization.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptFieldSerialization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SerializedObject.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SerializedObject.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SerializedProperty.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SerializedProperty.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptScheduler.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptScheduler.cpp
)

nls_add_runtime_target(
    NAME NLS_Scripting
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    LIBS
        NLS_Engine
        NLS_Core
        NLS_Math
        NLS_Base
)

target_compile_definitions(NLS_Scripting PUBLIC
    NLS_ENABLE_CSHARP_SCRIPTING=$<BOOL:${NLS_ENABLE_CSHARP_SCRIPTING}>
    NLS_ENABLE_LUA_SCRIPTING=$<BOOL:${NLS_ENABLE_LUA_SCRIPTING}>
    # LuaPanda is never compiled into non-Debug configurations, even when a
    # shared cache was configured with the opt-in switch enabled.
    NLS_ENABLE_LUAPANDA_DEBUGGING=$<IF:$<AND:$<BOOL:${NLS_ENABLE_LUAPANDA_DEBUGGING}>,$<CONFIG:Debug>>,1,0>
)

if(CMAKE_DL_LIBS)
    target_link_libraries(NLS_Scripting PRIVATE ${CMAKE_DL_LIBS})
endif()

if(NLS_ENABLE_LUA_SCRIPTING AND TARGET NLS_Lua)
    target_link_libraries(NLS_Scripting PRIVATE NLS_Lua)
    target_compile_definitions(NLS_Scripting PUBLIC NLS_HAS_LUA_VM=1)
else()
    target_compile_definitions(NLS_Scripting PUBLIC NLS_HAS_LUA_VM=0)
endif()
if(NLS_ENABLE_LUAPANDA_DEBUGGING AND TARGET NLS_LuaSocket)
    target_link_libraries(NLS_Scripting PRIVATE $<$<CONFIG:Debug>:NLS_LuaSocket>)
    file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/ThirdParty/LuaPanda/Debugger/LuaPanda.lua" _nls_luapanda_source_path)
    target_compile_definitions(NLS_Scripting PUBLIC
        NLS_HAS_LUAPANDA_SOCKET=$<IF:$<CONFIG:Debug>,1,0>
        NLS_LUAPANDA_SOURCE_PATH="${_nls_luapanda_source_path}")
else()
    target_compile_definitions(NLS_Scripting PUBLIC NLS_HAS_LUAPANDA_SOCKET=0)
endif()

if(NLS_ENABLE_CSHARP_SCRIPTING)
    # Declare these commands in the same CMake directory as NLS_Scripting so
    # Visual Studio attaches the custom build rules to that real project.
    include("${CMAKE_SOURCE_DIR}/Managed/ManagedBuild.cmake")
endif()
