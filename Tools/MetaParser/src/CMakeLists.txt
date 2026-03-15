set(_meta_parser_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/Main.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/Templates/MetaGenerated.cpp.tt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Templates/MetaGenerated.h.tt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Templates/HeaderGenerated.cpp.tt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Templates/HeaderGenerated.h.tt"
)

add_executable(MetaParser ${_meta_parser_sources})

set_target_properties(MetaParser PROPERTIES
    DOTNET_TARGET_FRAMEWORK "net8.0"
    VS_GLOBAL_RootNamespace "MetaParser"
    VS_GLOBAL_ImplicitUsings "enable"
    VS_GLOBAL_Nullable "enable"
    VS_GLOBAL_AppendTargetFrameworkToOutputPath "false"
    VS_GLOBAL_CopyLocalLockFileAssemblies "true"
    VS_PACKAGE_REFERENCES "Mono.TextTemplating_3.0.0"
    FOLDER Tools
)

if(TARGET CppAst)
    target_link_libraries(MetaParser PRIVATE CppAst)
endif()

foreach(_template IN ITEMS
    "${CMAKE_CURRENT_SOURCE_DIR}/Templates/MetaGenerated.cpp.tt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Templates/MetaGenerated.h.tt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Templates/HeaderGenerated.cpp.tt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Templates/HeaderGenerated.h.tt"
)
    get_filename_component(_template_name "${_template}" NAME)
    set_source_files_properties("${_template}" PROPERTIES
        VS_TOOL_OVERRIDE "None"
        VS_CSHARP_Link "Templates/${_template_name}"
        VS_CSHARP_CopyToOutputDirectory "PreserveNewest"
    )
endforeach()
