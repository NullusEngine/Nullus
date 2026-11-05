cmake_minimum_required(VERSION 3.22)

if(NOT NLS_DOTNET_EXECUTABLE)
    get_filename_component(NLS_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." REALPATH)
    include("${NLS_ROOT_DIR}/Tools/CMake/NullusDotnet.cmake")
    nls_resolve_dotnet()
endif()

set(NLS_META_PARSER_LIBCLANG_VERSION "18.1.3.2")
set(NLS_META_PARSER_LIBCLANGSHARP_VERSION "18.1.3.1")
set(NLS_META_PARSER_MODE "threads" CACHE STRING "MetaParser execution mode: serial, threads, or processes")
set_property(CACHE NLS_META_PARSER_MODE PROPERTY STRINGS serial threads processes)
set(NLS_META_PARSER_JOBS "0" CACHE STRING "MetaParser workers per module (0 selects up to four workers)")
option(NLS_META_PARSER_CACHE "Reuse reflection parsing results across targets and builds" ON)
set(NLS_META_PARSER_CACHE_DIR "${CMAKE_BINARY_DIR}/Tools/MetaParser/cache" CACHE PATH "Shared MetaParser input/model cache")
set(NLS_META_PARSER_TOTAL_JOBS "0" CACHE STRING "Native parsing workers across targets (0 selects up to eight workers)")
set(NLS_META_PARSER_TARGET_JOBS "0" CACHE STRING "Targets processed concurrently by the batch host (0 selects up to four)")
if(NOT NLS_META_PARSER_MODE MATCHES "^(serial|threads|processes)$")
    message(FATAL_ERROR "NLS_META_PARSER_MODE must be serial, threads, or processes.")
endif()
if(NOT NLS_META_PARSER_TARGET_JOBS MATCHES "^(0|[1-9][0-9]*)$" OR NLS_META_PARSER_TARGET_JOBS GREATER 2147483647)
    message(FATAL_ERROR "NLS_META_PARSER_TARGET_JOBS must be a nonnegative integer.")
endif()
if(NOT NLS_META_PARSER_JOBS MATCHES "^(0|[1-9][0-9]*)$" OR NLS_META_PARSER_JOBS GREATER 2147483647)
    message(FATAL_ERROR "NLS_META_PARSER_JOBS must be a nonnegative integer.")
endif()
if(NOT NLS_META_PARSER_TOTAL_JOBS MATCHES "^(0|[1-9][0-9]*)$" OR NLS_META_PARSER_TOTAL_JOBS GREATER 2147483647)
    message(FATAL_ERROR "NLS_META_PARSER_TOTAL_JOBS must be a nonnegative integer.")
endif()

if(NOT NLS_DOTNET_EXECUTABLE)
    message(FATAL_ERROR "NLS_DOTNET_EXECUTABLE must resolve to the repository's .NET 8 SDK before configuring MetaParser.")
endif()

set(DOTNET_EXECUTABLE "${NLS_DOTNET_EXECUTABLE}")
set(NLS_META_PARSER_DOTNET "${NLS_DOTNET_EXECUTABLE}" CACHE INTERNAL "dotnet executable" FORCE)
if(WIN32)
    set(NLS_DOTNET_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/../../Tools/RunRepositoryDotnet.cmd")
else()
    set(NLS_DOTNET_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/../../Tools/RunRepositoryDotnet.sh")
endif()

set(NLS_META_PARSER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(NLS_META_PARSER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(NLS_META_PARSER_INTERMEDIATE_DIR "${NLS_META_PARSER_GENERATED_DIR}/src")
set(NLS_META_PARSER_PROJECT_TEMPLATE "${NLS_META_PARSER_SOURCE_DIR}/MetaParser.csproj.in")
set(NLS_META_PARSER_PROJECT "${NLS_META_PARSER_INTERMEDIATE_DIR}/MetaParser.csproj" CACHE INTERNAL "MetaParser csproj" FORCE)
set(NLS_META_PARSER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/publish/$<CONFIG>" CACHE INTERNAL "MetaParser output directory" FORCE)
set(NLS_META_PARSER_NUGET_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/LocalNuget;https://api.nuget.org/v3/index.json")
set(NLS_META_PARSER_NUGET_PACKAGES "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/.nuget/packages" CACHE PATH "Repository-local MetaParser NuGet extraction cache")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
    set(NLS_META_PARSER_HOST_ARCH "arm64")
else()
    set(NLS_META_PARSER_HOST_ARCH "x64")
endif()

if(WIN32)
    set(NLS_META_PARSER_RUNTIME_ID "win-${NLS_META_PARSER_HOST_ARCH}")
    set(NLS_META_PARSER_EXECUTABLE "${NLS_META_PARSER_OUTPUT_DIR}/MetaParser.exe" CACHE INTERNAL "MetaParser executable path" FORCE)
elseif(APPLE)
    set(NLS_META_PARSER_RUNTIME_ID "osx-${NLS_META_PARSER_HOST_ARCH}")
    set(NLS_META_PARSER_EXECUTABLE "${NLS_META_PARSER_OUTPUT_DIR}/MetaParser" CACHE INTERNAL "MetaParser executable path" FORCE)
else()
    set(NLS_META_PARSER_RUNTIME_ID "linux-${NLS_META_PARSER_HOST_ARCH}")
    set(NLS_META_PARSER_EXECUTABLE "${NLS_META_PARSER_OUTPUT_DIR}/MetaParser" CACHE INTERNAL "MetaParser executable path" FORCE)
endif()

set(NLS_META_PARSER_CPPAST_CSPROJ "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/CppAst.NET/src/CppAst/CppAst.csproj")
set(NLS_META_PARSER_TEMPLATES_DIR "${NLS_META_PARSER_SOURCE_DIR}/Templates")
set(NLS_META_PARSER_TEMPLATE_COMPILER "${CMAKE_CURRENT_SOURCE_DIR}/TemplateCompiler/TemplateCompiler.csproj")
set(NLS_META_PARSER_TEMPLATE_COMPILER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/TemplateCompiler/Program.cs")
set(NLS_META_PARSER_LAUNCH_SETTINGS_DIR "${NLS_META_PARSER_INTERMEDIATE_DIR}/Properties")
set(NLS_META_PARSER_LAUNCH_SETTINGS_TEMPLATE "${NLS_META_PARSER_SOURCE_DIR}/Properties/launchSettings.json.in")
set(NLS_META_PARSER_LAUNCH_SETTINGS "${NLS_META_PARSER_LAUNCH_SETTINGS_DIR}/launchSettings.json")
set(NLS_META_PARSER_DEFAULT_PRECOMPILE_BASE "${CMAKE_BINARY_DIR}/Runtime/Base/NLS_Base.precompile.json")
set(NLS_META_PARSER_DEFAULT_PRECOMPILE_CORE "${CMAKE_BINARY_DIR}/Runtime/Core/NLS_Core.precompile.json")

file(GLOB NLS_META_PARSER_TEMPLATE_FILES
    CONFIGURE_DEPENDS
    "${NLS_META_PARSER_TEMPLATES_DIR}/*.tt"
)
file(GLOB_RECURSE NLS_META_PARSER_SOURCE_FILES
    CONFIGURE_DEPENDS
    "${NLS_META_PARSER_SOURCE_DIR}/*.cs"
)
file(GLOB_RECURSE NLS_META_PARSER_CPPAST_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/CppAst.NET/src/CppAst/*.cs"
)
list(FILTER NLS_META_PARSER_SOURCE_FILES EXCLUDE REGEX [[/bin/]])
list(FILTER NLS_META_PARSER_SOURCE_FILES EXCLUDE REGEX [[/obj/]])
list(FILTER NLS_META_PARSER_CPPAST_SOURCE_FILES EXCLUDE REGEX [[/bin/]])
list(FILTER NLS_META_PARSER_CPPAST_SOURCE_FILES EXCLUDE REGEX [[/obj/]])

file(MAKE_DIRECTORY "${NLS_META_PARSER_INTERMEDIATE_DIR}")
file(MAKE_DIRECTORY "${NLS_META_PARSER_LAUNCH_SETTINGS_DIR}")
configure_file("${NLS_META_PARSER_PROJECT_TEMPLATE}" "${NLS_META_PARSER_PROJECT}" @ONLY)
configure_file("${NLS_META_PARSER_LAUNCH_SETTINGS_TEMPLATE}" "${NLS_META_PARSER_LAUNCH_SETTINGS}" @ONLY)

# The CMake target below is intentionally kept as the hermetic build wrapper
# used by the engine.  Expose the generated SDK-style project separately in a
# Visual Studio solution so MetaParser's real C# sources and breakpoints are
# visible in the IDE.
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    include_external_msproject(MetaParserSource "${NLS_META_PARSER_PROJECT}" PLATFORM AnyCPU)
    set_property(TARGET MetaParserSource PROPERTY FOLDER Tools)
endif()

# The SDK project owns template preprocessing for every generator.
project(MetaParser LANGUAGES NONE)

    set(NLS_META_PARSER_STAMP "${CMAKE_CURRENT_BINARY_DIR}/MetaParser_$<CONFIG>.stamp")

    add_custom_command(
        OUTPUT "${NLS_META_PARSER_STAMP}"
        # Invoke the repository-local SDK by absolute path.  Do not rewrite
        # PATH here: Visual Studio can preserve a differently-cased Path key,
        # and cmake -E env would then create duplicate Windows environment
        # entries that MSBuild rejects.
        COMMAND "${NLS_DOTNET_RUNNER}"
            "${DOTNET_EXECUTABLE}" publish "${NLS_META_PARSER_PROJECT}" --configuration "$<CONFIG>" --runtime "${NLS_META_PARSER_RUNTIME_ID}" --self-contained false --nologo -o "${NLS_META_PARSER_OUTPUT_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${NLS_META_PARSER_STAMP}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS
            "${NLS_META_PARSER_PROJECT}"
            "${NLS_META_PARSER_LAUNCH_SETTINGS}"
            ${NLS_META_PARSER_SOURCE_FILES}
            ${NLS_META_PARSER_CPPAST_SOURCE_FILES}
            ${NLS_META_PARSER_TEMPLATE_FILES}
            "${NLS_META_PARSER_TEMPLATE_COMPILER}"
            "${NLS_META_PARSER_TEMPLATE_COMPILER_SOURCE}"
            "${NLS_META_PARSER_CPPAST_CSPROJ}"
        BYPRODUCTS "${NLS_META_PARSER_EXECUTABLE}"
        COMMENT "Building C# MetaParser via generated SDK project"
        VERBATIM
    )

    add_custom_target(MetaParser ALL
        DEPENDS "${NLS_META_PARSER_STAMP}"
    )
set_target_properties(MetaParser PROPERTIES
    FOLDER CMakePredefinedTargets
    # The generated SDK project is also named MetaParser.  Give the CMake
    # wrapper a distinct Visual Studio label while keeping its target name and
    # build dependencies unchanged.
    PROJECT_LABEL "MetaParserBuild"
)

function(nls_define_meta_batch_target)
    get_property(_candidates GLOBAL PROPERTY NLS_META_BATCH_TARGETS)
    if(NOT _candidates)
        return()
    endif()
    set(_configs "")
    set(_stamps "")
    set(_dependencies "")
    foreach(_target IN LISTS _candidates)
        get_target_property(_excluded ${_target} EXCLUDE_FROM_ALL)
        get_target_property(_source ${_target} SOURCE_DIR)
        while(_source AND NOT _excluded)
            get_property(_excluded DIRECTORY "${_source}" PROPERTY EXCLUDE_FROM_ALL)
            get_property(_source DIRECTORY "${_source}" PROPERTY PARENT_DIRECTORY)
        endwhile()
        if(_excluded)
            continue()
        endif()
        get_target_property(_params ${_target} NLS_META_BATCH_PARAMS)
        get_target_property(_stamp ${_target} NLS_META_BATCH_STAMP)
        get_target_property(_dependency ${_target} NLS_META_BATCH_DEPENDENCY)
        list(APPEND _configs "${_params}")
        list(APPEND _stamps "${_stamp}")
        list(APPEND _dependencies "${_dependency}")
        if(NOT _command)
            get_target_property(_command ${_target} NLS_META_BATCH_COMMAND)
            get_target_property(_environment ${_target} NLS_META_BATCH_ENVIRONMENT)
        endif()
    endforeach()
    if(NOT _configs)
        return()
    endif()
    list(REMOVE_DUPLICATES _dependencies)
    nls_join_json_array(_configs_json ${_configs})
    set(_manifest "${CMAKE_BINARY_DIR}/Tools/MetaParser/ReflectionBatch.json")
    file(GENERATE OUTPUT "${_manifest}"
        CONTENT "{\n  \"Configs\": [${_configs_json}],\n  \"TargetJobs\": ${NLS_META_PARSER_TARGET_JOBS}\n}\n")
    add_custom_target(NullusReflectionBatch
        COMMAND "${CMAKE_COMMAND}" -E env ${_environment} "${_command}" --batch "${_manifest}"
        COMMAND "${CMAKE_COMMAND}" -E touch ${_stamps}
        DEPENDS ${_configs}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating reflection for configured targets in one MetaParser host"
        VERBATIM)
    add_dependencies(NullusReflectionBatch ${_dependencies})
    set_target_properties(NullusReflectionBatch PROPERTIES FOLDER "CMakePredefinedTargets/Reflection")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL nls_define_meta_batch_target)
