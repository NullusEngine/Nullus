set(_meta_parser_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/Main.cs"
)

add_executable(MetaParser ${_meta_parser_sources})

set_target_properties(MetaParser PROPERTIES
    DOTNET_TARGET_FRAMEWORK "net8.0"
    VS_GLOBAL_RootNamespace "MetaParser"
    VS_GLOBAL_ImplicitUsings "enable"
    VS_GLOBAL_Nullable "enable"
    VS_GLOBAL_CopyLocalLockFileAssemblies "true"
    VS_PACKAGE_REFERENCES "Mono.TextTemplating_3.0.0"
)

if(TARGET CppAst)
    target_link_libraries(MetaParser PRIVATE CppAst)
endif()

add_custom_command(TARGET MetaParser POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MetaParser>/Templates"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Templates/MetaGenerated.cpp.tt"
        "$<TARGET_FILE_DIR:MetaParser>/Templates/MetaGenerated.cpp.tt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Templates/MetaGenerated.h.tt"
        "$<TARGET_FILE_DIR:MetaParser>/Templates/MetaGenerated.h.tt"
    VERBATIM
)
