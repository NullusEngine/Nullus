file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.cs)

set(CMAKE_CSharp_FLAGS "${CMAKE_CSharp_FLAGS} /langversion:10.0")

add_executable(MetaParser ${SOURCE_FILES})

if (WIN32)
    set(RID "win-x64")
elseif(APPLE)
    set(RID "osx-x64")
elseif(UNIX)
    set(RID "linux-x64")
endif()

set_target_properties(MetaParser PROPERTIES
    VS_PACKAGE_REFERENCES "System.CommandLine_2.0.0-rc.1.25451.107;System.CodeDom_10.0.0-rc.1.25451.107"
)

set_property(TARGET MetaParser PROPERTY VS_GLOBAL_RuntimeIdentifier "${RID}")

target_compile_options(MetaParser PUBLIC "/unsafe")
target_link_libraries(MetaParser CppAst)