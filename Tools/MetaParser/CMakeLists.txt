cmake_minimum_required(VERSION 3.22)

set(NLS_META_PARSER_LIBCLANG_VERSION "18.1.3.2")
set(NLS_META_PARSER_LIBCLANGSHARP_VERSION "18.1.3.1")

find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
if(NOT DOTNET_EXECUTABLE)
    message(FATAL_ERROR "dotnet SDK is required for C# MetaParser. Please install .NET 8 SDK.")
endif()

set(NLS_META_PARSER_DOTNET "${DOTNET_EXECUTABLE}" CACHE INTERNAL "dotnet executable" FORCE)

set(NLS_META_PARSER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(NLS_META_PARSER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(NLS_META_PARSER_INTERMEDIATE_DIR "${NLS_META_PARSER_GENERATED_DIR}/src")
set(NLS_META_PARSER_PROJECT_TEMPLATE "${NLS_META_PARSER_SOURCE_DIR}/MetaParser.csproj.in")
set(NLS_META_PARSER_PROJECT "${NLS_META_PARSER_INTERMEDIATE_DIR}/MetaParser.csproj" CACHE INTERNAL "MetaParser csproj" FORCE)
set(NLS_META_PARSER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/publish/$<CONFIG>" CACHE INTERNAL "MetaParser output directory" FORCE)

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
set(NLS_META_PARSER_LAUNCH_SETTINGS_DIR "${NLS_META_PARSER_INTERMEDIATE_DIR}/Properties")
set(NLS_META_PARSER_LAUNCH_SETTINGS_TEMPLATE "${NLS_META_PARSER_SOURCE_DIR}/Properties/launchSettings.json.in")
set(NLS_META_PARSER_LAUNCH_SETTINGS "${NLS_META_PARSER_LAUNCH_SETTINGS_DIR}/launchSettings.json")
set(NLS_META_PARSER_DEFAULT_PRECOMPILE_BASE "${CMAKE_BINARY_DIR}/Runtime/Base/NLS_Base.precompile.json")
set(NLS_META_PARSER_DEFAULT_PRECOMPILE_CORE "${CMAKE_BINARY_DIR}/Runtime/Core/NLS_Core.precompile.json")

file(MAKE_DIRECTORY "${NLS_META_PARSER_INTERMEDIATE_DIR}")
file(MAKE_DIRECTORY "${NLS_META_PARSER_LAUNCH_SETTINGS_DIR}")
configure_file("${NLS_META_PARSER_PROJECT_TEMPLATE}" "${NLS_META_PARSER_PROJECT}" @ONLY)
configure_file("${NLS_META_PARSER_LAUNCH_SETTINGS_TEMPLATE}" "${NLS_META_PARSER_LAUNCH_SETTINGS}" @ONLY)

set(NLS_META_PARSER_WINDOWS_NATIVE FALSE)
if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
    set(NLS_META_PARSER_WINDOWS_NATIVE TRUE)
endif()

if(NLS_META_PARSER_WINDOWS_NATIVE)
    project(MetaParser LANGUAGES CSharp)

    set(CMAKE_CSharp_FLAGS "${CMAKE_CSharp_FLAGS} /langversion:latest")
    set(CMAKE_DOTNET_TARGET_FRAMEWORK "net8.0")
    set(CMAKE_DOTNET_SDK "Microsoft.NET.Sdk")

    add_subdirectory(ThirdParty)
    add_subdirectory(src)
    set(NLS_META_PARSER_EXECUTABLE "${CMAKE_CURRENT_BINARY_DIR}/src/$<CONFIG>/MetaParser.exe" CACHE INTERNAL "MetaParser executable path" FORCE)

    # On Windows with VS generator, use dotnet build instead of publish
    # Template copying is handled via VS_POST_BUILD_EVENT_COMMAND in .csproj
else()
    project(MetaParser LANGUAGES NONE)

    add_custom_target(MetaParser ALL
        COMMAND "${DOTNET_EXECUTABLE}" publish "${NLS_META_PARSER_PROJECT}" --configuration "$<CONFIG>" --runtime "${NLS_META_PARSER_RUNTIME_ID}" --self-contained false --nologo -o "${NLS_META_PARSER_OUTPUT_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS
            "${NLS_META_PARSER_PROJECT}"
            "${NLS_META_PARSER_LAUNCH_SETTINGS}"
            "${NLS_META_PARSER_SOURCE_DIR}/Main.cs"
            "${NLS_META_PARSER_TEMPLATES_DIR}/MetaGenerated.cpp.tt"
            "${NLS_META_PARSER_TEMPLATES_DIR}/MetaGenerated.h.tt"
            "${NLS_META_PARSER_CPPAST_CSPROJ}"
        COMMENT "Building C# MetaParser via generated SDK project"
        VERBATIM
    )
endif()

set_target_properties(MetaParser PROPERTIES FOLDER Tools)
