set(_meta_parser_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/Main.cs"
)

add_executable(MetaParserManaged ${_meta_parser_sources})

set_target_properties(MetaParserManaged PROPERTIES
    DOTNET_TARGET_FRAMEWORK "net8.0"
    VS_GLOBAL_RootNamespace "MetaParser"
    VS_GLOBAL_ImplicitUsings "enable"
    VS_GLOBAL_Nullable "enable"
    VS_GLOBAL_CopyLocalLockFileAssemblies "true"
    VS_PACKAGE_REFERENCES "Mono.TextTemplating_3.0.0"
    FOLDER Tools
)

if(TARGET CppAst)
    target_link_libraries(MetaParserManaged PRIVATE CppAst)
endif()
