# Resolve the one .NET SDK used by MetaParser, generators, and game scripts.
# The repository-local installation is preferred and never changes the host PATH.

set(NLS_DOTNET_VERSION "8.0.408" CACHE STRING "Pinned .NET SDK version for Nullus scripting and code generation")
option(NLS_AUTO_INSTALL_DOTNET "Download the pinned repository-local .NET SDK when it is missing" ON)
if(NOT NLS_ROOT_DIR)
    set(NLS_ROOT_DIR "${CMAKE_SOURCE_DIR}")
endif()

function(_nls_dotnet_check_candidate CANDIDATE OUT_EXECUTABLE OUT_VERSION)
    set(${OUT_EXECUTABLE} "" PARENT_SCOPE)
    set(${OUT_VERSION} "" PARENT_SCOPE)
    if(NOT CANDIDATE OR NOT EXISTS "${CANDIDATE}")
        return()
    endif()

    execute_process(
        COMMAND "${CANDIDATE}" --version
        RESULT_VARIABLE _nls_dotnet_result
        OUTPUT_VARIABLE _nls_dotnet_version
        ERROR_VARIABLE _nls_dotnet_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
        TIMEOUT 30)
    string(REGEX REPLACE "\\." "\\\\." _nls_expected_version_regex "${NLS_DOTNET_VERSION}")
    if(_nls_dotnet_result EQUAL 0 AND _nls_dotnet_version MATCHES "^${_nls_expected_version_regex}$")
        set(${OUT_EXECUTABLE} "${CANDIDATE}" PARENT_SCOPE)
        set(${OUT_VERSION} "${_nls_dotnet_version}" PARENT_SCOPE)
    endif()
endfunction()

function(nls_resolve_dotnet)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(_nls_dotnet_arch "arm64")
    else()
        set(_nls_dotnet_arch "x64")
    endif()

    if(WIN32)
        set(_nls_dotnet_platform "windows")
        set(_nls_dotnet_name "dotnet.exe")
    elseif(APPLE)
        set(_nls_dotnet_platform "macos")
        set(_nls_dotnet_name "dotnet")
    else()
        set(_nls_dotnet_platform "linux")
        set(_nls_dotnet_name "dotnet")
    endif()

    set(_nls_candidates)
    if(NLS_DOTNET_EXECUTABLE)
        list(APPEND _nls_candidates "${NLS_DOTNET_EXECUTABLE}")
    endif()
    if(DEFINED ENV{NLS_DOTNET_EXECUTABLE} AND NOT "$ENV{NLS_DOTNET_EXECUTABLE}" STREQUAL "")
        list(APPEND _nls_candidates "$ENV{NLS_DOTNET_EXECUTABLE}")
    endif()
    if(NLS_DOTNET_ROOT)
        list(APPEND _nls_candidates "${NLS_DOTNET_ROOT}/${_nls_dotnet_name}")
    endif()
    if(DEFINED ENV{NLS_DOTNET_ROOT} AND NOT "$ENV{NLS_DOTNET_ROOT}" STREQUAL "")
        list(APPEND _nls_candidates "$ENV{NLS_DOTNET_ROOT}/${_nls_dotnet_name}")
    endif()
    list(APPEND _nls_candidates
        "${NLS_ROOT_DIR}/Tools/Dotnet/${_nls_dotnet_platform}/${_nls_dotnet_arch}/${_nls_dotnet_name}")

    find_program(_nls_system_dotnet dotnet)
    if(_nls_system_dotnet)
        list(APPEND _nls_candidates "${_nls_system_dotnet}")
    endif()
    list(REMOVE_DUPLICATES _nls_candidates)

    set(_nls_dotnet_resolved "")
    set(_nls_dotnet_resolved_version "")
    foreach(_nls_candidate IN LISTS _nls_candidates)
        _nls_dotnet_check_candidate(
            "${_nls_candidate}" _nls_candidate_executable _nls_candidate_version)
        if(_nls_candidate_executable)
            set(_nls_dotnet_resolved "${_nls_candidate_executable}")
            set(_nls_dotnet_resolved_version "${_nls_candidate_version}")
            break()
        endif()
    endforeach()

    if(NOT _nls_dotnet_resolved AND NLS_AUTO_INSTALL_DOTNET)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            message(STATUS "Pinned .NET ${NLS_DOTNET_VERSION} SDK not found; bootstrapping repository-local SDK")
            execute_process(
                COMMAND "${Python3_EXECUTABLE}"
                    "${NLS_ROOT_DIR}/Tools/SetupDependencies/setup_dependencies.py"
                    --dependency dotnet-sdk
                    --platform "${_nls_dotnet_platform}"
                    --arch "${_nls_dotnet_arch}"
                    --non-interactive
                    --repo-root "${NLS_ROOT_DIR}"
                RESULT_VARIABLE _nls_bootstrap_result
                OUTPUT_VARIABLE _nls_bootstrap_output
                ERROR_VARIABLE _nls_bootstrap_error
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT _nls_bootstrap_result EQUAL 0)
                message(WARNING
                    "Automatic .NET SDK bootstrap failed (exit ${_nls_bootstrap_result}):\n"
                    "${_nls_bootstrap_output}\n${_nls_bootstrap_error}")
            endif()
        else()
            message(WARNING "NLS_AUTO_INSTALL_DOTNET is ON but Python 3.8+ was not found.")
        endif()

        set(_nls_local_dotnet
            "${NLS_ROOT_DIR}/Tools/Dotnet/${_nls_dotnet_platform}/${_nls_dotnet_arch}/${_nls_dotnet_name}")
        _nls_dotnet_check_candidate(
            "${_nls_local_dotnet}" _nls_dotnet_resolved _nls_dotnet_resolved_version)
    endif()

    if(NOT _nls_dotnet_resolved)
        message(FATAL_ERROR
            "Nullus requires .NET SDK 8 for MetaParser, generators, and C# scripts.\n"
            "Install automatically with:\n"
            "  SetupDependencies.bat --dependency dotnet-sdk\n"
            "  ./SetupDependencies.sh --dependency dotnet-sdk\n"
            "Or set NLS_DOTNET_EXECUTABLE to a .NET 8 SDK and configure with -DNLS_AUTO_INSTALL_DOTNET=OFF.")
    endif()

    set(NLS_DOTNET_EXECUTABLE "${_nls_dotnet_resolved}" CACHE FILEPATH "Resolved .NET 8 executable" FORCE)
    get_filename_component(_nls_dotnet_root "${_nls_dotnet_resolved}" DIRECTORY)
    set(NLS_DOTNET_ROOT "${_nls_dotnet_root}" CACHE PATH "Resolved .NET 8 SDK root" FORCE)
    set(NLS_DOTNET_VERSION_RESOLVED "${_nls_dotnet_resolved_version}" CACHE INTERNAL "Resolved .NET SDK version" FORCE)
    message(STATUS "Nullus .NET SDK: ${NLS_DOTNET_VERSION_RESOLVED} (${NLS_DOTNET_EXECUTABLE})")
endfunction()
