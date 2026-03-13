set(NLS_META_LIBCLANG_VERSION "18.1.3.2")
set(NLS_META_LIBCLANG_OSX_VERSION "18.1.3")
set(NLS_META_LIBCLANGSHARP_VERSION "18.1.3.1")

function(nls_collect_runtime_files OUT_HEADERS OUT_SOURCES SOURCE_DIR)
    file(GLOB_RECURSE _headers CONFIGURE_DEPENDS
      ${SOURCE_DIR}/*.h
      ${SOURCE_DIR}/*.hpp
      ${SOURCE_DIR}/*.inl
    )
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS
      ${SOURCE_DIR}/*.cpp
      ${SOURCE_DIR}/*.c
    )

    list(FILTER _headers EXCLUDE REGEX "/Gen/")
    list(FILTER _sources EXCLUDE REGEX "/Gen/")

    set(${OUT_HEADERS} ${_headers} PARENT_SCOPE)
    set(${OUT_SOURCES} ${_sources} PARENT_SCOPE)
endfunction()

function(nls_json_quote OUT_VAR INPUT)
    set(_value "${INPUT}")
    string(REPLACE "\\" "\\\\" _value "${_value}")
    string(REPLACE "\"" "\\\"" _value "${_value}")
    set(${OUT_VAR} "\"${_value}\"" PARENT_SCOPE)
endfunction()

function(nls_join_json_array OUT_VAR)
    set(_items "")
    foreach(_item IN LISTS ARGN)
        if(NOT "${_item}" STREQUAL "")
            nls_json_quote(_quoted "${_item}")
            list(APPEND _items "${_quoted}")
        endif()
    endforeach()

    if(_items)
        list(JOIN _items ", " _joined)
        set(${OUT_VAR} "${_joined}" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "" PARENT_SCOPE)
    endif()
endfunction()

function(nls_choose_meta_parser_compiler OUT_COMPILER OUT_TARGET OUT_SYSTEM_INCLUDES)
    set(_compiler "${CMAKE_CXX_COMPILER}")
    set(_target "")
    set(_includes "")

    if(NOT WIN32)
        find_program(_meta_clangxx NAMES clang++ clang)
        if(_meta_clangxx)
            set(_compiler "${_meta_clangxx}")
        endif()
    endif()

    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(_target "aarch64-pc-windows-msvc")
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_target "x86_64-pc-windows-msvc")
        else()
            set(_target "i686-pc-windows-msvc")
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_target "aarch64-apple-darwin")
        else()
            set(_target "x86_64-apple-darwin")
        endif()
    elseif(UNIX)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(_target "aarch64-linux-gnu")
        else()
            set(_target "x86_64-linux-gnu")
        endif()

        nls_detect_compiler_system_includes("${_compiler}" _detected_linux_includes)
        if(_detected_linux_includes)
            list(APPEND _includes ${_detected_linux_includes})
        else()
            foreach(_dir
                "/usr/include/c++/9"
                "/usr/include/x86_64-linux-gnu/c++/9"
                "/usr/include/c++/9/backward"
                "/usr/lib/gcc/x86_64-linux-gnu/9/include"
                "/usr/local/include"
                "/usr/include/x86_64-linux-gnu"
                "/usr/include")
                if(EXISTS "${_dir}")
                    list(APPEND _includes "${_dir}")
                endif()
            endforeach()
        endif()
    endif()

    list(REMOVE_DUPLICATES _includes)
    set(${OUT_COMPILER} "${_compiler}" PARENT_SCOPE)
    set(${OUT_TARGET} "${_target}" PARENT_SCOPE)
    set(${OUT_SYSTEM_INCLUDES} ${_includes} PARENT_SCOPE)
endfunction()

function(nls_detect_compiler_builtin_defines COMPILER OUT_DEFINES)
    set(_defines "")

    if("${COMPILER}" AND EXISTS "${COMPILER}" AND NOT MSVC)
        if("${COMPILER}" MATCHES "g..-9$")
            set(_meta_std_flag "-std=c++2a")
        else()
            set(_meta_std_flag "-std=c++20")
        endif()

        execute_process(
            COMMAND "${COMPILER}" -dM -E -x c++ ${_meta_std_flag} /dev/null
            OUTPUT_VARIABLE _macro_output
            ERROR_QUIET
            RESULT_VARIABLE _macro_result
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(_macro_result EQUAL 0)
            string(REPLACE "\r" "" _macro_output "${_macro_output}")
            string(REPLACE "\n" ";" _macro_lines "${_macro_output}")
            foreach(_line IN LISTS _macro_lines)
                if(_line MATCHES "^#define (__GCC_ATOMIC_[A-Za-z0-9_]+|__GNUC__|__GNUC_MINOR__|__GNUC_PATCHLEVEL__|__cplusplus|__STDC__|__STDC_HOSTED__|__STRICT_ANSI__|__SIZEOF_FLOAT128__|__FINITE_MATH_ONLY__|_GNU_SOURCE) (.+)$")
                    set(_name "${CMAKE_MATCH_1}")
                    set(_value "${CMAKE_MATCH_2}")
                    string(STRIP "${_value}" _value)
                    list(APPEND _defines "${_name}=${_value}")
                endif()
            endforeach()
        endif()
    endif()

    list(REMOVE_DUPLICATES _defines)
    set(${OUT_DEFINES} ${_defines} PARENT_SCOPE)
endfunction()

function(nls_detect_compiler_system_includes COMPILER OUT_INCLUDES)
    set(_includes "")

    if("${COMPILER}" AND EXISTS "${COMPILER}" AND NOT MSVC)
        if("${COMPILER}" MATCHES "g..-9$")
            set(_meta_std_flag "-std=c++2a")
        else()
            set(_meta_std_flag "-std=c++20")
        endif()

        execute_process(
            COMMAND "${COMPILER}" -E -x c++ ${_meta_std_flag} - -v
            INPUT_FILE /dev/null
            OUTPUT_QUIET
            ERROR_VARIABLE _compiler_verbose
            RESULT_VARIABLE _compiler_verbose_result
        )

        if(_compiler_verbose_result EQUAL 0)
            string(REPLACE "\r" "" _compiler_verbose "${_compiler_verbose}")
            string(REGEX MATCHALL "(^|\n) ([^\n]+)" _include_matches "${_compiler_verbose}")
            foreach(_match IN LISTS _include_matches)
                string(REGEX REPLACE "(^|\n) " "" _line "${_match}")
                string(STRIP "${_line}" _line)
                string(REGEX REPLACE " \(framework directory\)$" "" _line "${_line}")
                if(_line AND EXISTS "${_line}")
                    list(APPEND _includes "${_line}")
                endif()
            endforeach()
        endif()

        if(NOT _includes AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            execute_process(
                COMMAND "${COMPILER}" -dumpfullversion
                OUTPUT_VARIABLE _gcc_version
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(NOT _gcc_version)
                execute_process(
                    COMMAND "${COMPILER}" -dumpversion
                    OUTPUT_VARIABLE _gcc_version
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
            endif()
            string(REGEX MATCH "^[0-9]+" _gcc_major "${_gcc_version}")
            execute_process(
                COMMAND "${COMPILER}" -print-multiarch
                OUTPUT_VARIABLE _gcc_multiarch
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            execute_process(
                COMMAND "${COMPILER}" -print-file-name=include
                OUTPUT_VARIABLE _gcc_builtin_include
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            foreach(_dir
                "${_gcc_builtin_include}"
                "/usr/include/c++/${_gcc_major}"
                "/usr/include/${_gcc_multiarch}/c++/${_gcc_major}"
                "/usr/include/c++/${_gcc_major}/backward"
                "/usr/lib/gcc/${_gcc_multiarch}/${_gcc_major}/include"
                "/usr/local/include"
                "/usr/include/${_gcc_multiarch}"
                "/usr/include")
                if(_dir AND EXISTS "${_dir}")
                    list(APPEND _includes "${_dir}")
                endif()
            endforeach()
        endif()
    endif()

    list(REMOVE_DUPLICATES _includes)
    set(${OUT_INCLUDES} ${_includes} PARENT_SCOPE)
endfunction()

function(nls_detect_apple_sysroot OUT_SYSROOT)
    set(_sysroot "")
    if(APPLE)
        execute_process(
            COMMAND xcrun --sdk macosx --show-sdk-path
            OUTPUT_VARIABLE _xcrun_sysroot
            ERROR_QUIET
            RESULT_VARIABLE _xcrun_result
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_xcrun_result EQUAL 0 AND EXISTS "${_xcrun_sysroot}")
            set(_sysroot "${_xcrun_sysroot}")
        endif()
    endif()
    set(${OUT_SYSROOT} "${_sysroot}" PARENT_SCOPE)
endfunction()

function(nls_detect_clang_resource_dir OUT_RESOURCE_DIR)
    set(options)
    set(oneValueArgs COMPILER)
    cmake_parse_arguments(NLS "${options}" "${oneValueArgs}" "" ${ARGN})

    set(_resource_dir "")
    set(_clangxx_program "${NLS_COMPILER}")
    if(NOT _clangxx_program)
        find_program(_clangxx_program NAMES clang++ clang)
    endif()
    if(_clangxx_program)
        execute_process(
            COMMAND "${_clangxx_program}" -print-resource-dir
            OUTPUT_VARIABLE _clang_resource_dir
            ERROR_QUIET
            RESULT_VARIABLE _clang_resource_result
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_clang_resource_result EQUAL 0 AND EXISTS "${_clang_resource_dir}")
            set(_resource_dir "${_clang_resource_dir}")
        endif()
    endif()
    set(${OUT_RESOURCE_DIR} "${_resource_dir}" PARENT_SCOPE)
endfunction()

function(nls_get_common_meta_include_dirs OUT_VAR)
    set(_dirs
        "${NLS_ROOT_DIR}/Runtime"
        "${NLS_ThirdParty_DIR}"
    )

    file(GLOB _runtime_children LIST_DIRECTORIES true "${NLS_ROOT_DIR}/Runtime/*")
    foreach(_dir ${_runtime_children})
        if(IS_DIRECTORY "${_dir}")
            list(APPEND _dirs "${_dir}")
        endif()
    endforeach()

    list(REMOVE_DUPLICATES _dirs)
    set(${OUT_VAR} ${_dirs} PARENT_SCOPE)
endfunction()

function(nls_add_target)
    set(options)
    set(oneValueArgs NAME CATEGORY SOURCE_DIR)
    set(multiValueArgs LIBS APPEND_INCLUDE_DIR)

    cmake_parse_arguments(TARGET "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    nls_collect_runtime_files(HEAD_FILES SRC_FILES "${TARGET_SOURCE_DIR}")

    source_group(TREE ${TARGET_SOURCE_DIR} FILES ${HEAD_FILES})
    source_group(TREE ${TARGET_SOURCE_DIR} FILES ${SRC_FILES})

    if(NLS_USE_SHARED_LIBRARY)
      add_library(${TARGET_NAME} SHARED ${HEAD_FILES} ${SRC_FILES})

      string(TOUPPER ${TARGET_NAME} NLS_EXPORT_MACRO)
      string(APPEND NLS_EXPORT_MACRO "_EXPORT")

      target_compile_definitions(
        ${TARGET_NAME}
        PRIVATE
          ${NLS_EXPORT_MACRO}
      )
    else()
      add_library(${TARGET_NAME} STATIC ${HEAD_FILES} ${SRC_FILES})
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
      FOLDER ${TARGET_CATEGORY}
    )

    target_include_directories(
      ${TARGET_NAME}
      PUBLIC
        $<BUILD_INTERFACE:${TARGET_SOURCE_DIR}>;
        $<BUILD_INTERFACE:${NLS_ROOT_DIR}/Runtime>;
        ${TARGET_APPEND_INCLUDE_DIR}
    )

    target_link_libraries(
      ${TARGET_NAME}
      PUBLIC
        ${TARGET_LIBS}
    )
endfunction()

function(nls_add_runtime_target)
    set(options)
    set(oneValueArgs NAME SOURCE_DIR)
    set(multiValueArgs LIBS APPEND_INCLUDE_DIR)

    cmake_parse_arguments(TARGET "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    nls_collect_runtime_files(HEAD_FILES SRC_FILES "${TARGET_SOURCE_DIR}")

    source_group(TREE ${TARGET_SOURCE_DIR} FILES ${HEAD_FILES})
    source_group(TREE ${TARGET_SOURCE_DIR} FILES ${SRC_FILES})

    if(NLS_USE_SHARED_LIBRARY)
      add_library(${TARGET_NAME} SHARED ${HEAD_FILES} ${SRC_FILES})

      string(TOUPPER ${TARGET_NAME} NLS_EXPORT_MACRO)
      string(APPEND NLS_EXPORT_MACRO "_EXPORT")

      target_compile_definitions(
        ${TARGET_NAME}
        PRIVATE
          ${NLS_EXPORT_MACRO}
      )
    else()
      add_library(${TARGET_NAME} STATIC ${HEAD_FILES} ${SRC_FILES})
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
      FOLDER Runtime
    )

    target_include_directories(
      ${TARGET_NAME}
      PUBLIC
        $<BUILD_INTERFACE:${TARGET_SOURCE_DIR}>;
        $<BUILD_INTERFACE:${NLS_ROOT_DIR}/Runtime>;
        ${TARGET_APPEND_INCLUDE_DIR}
    )

    target_link_libraries(
      ${TARGET_NAME}
      PUBLIC
        ${TARGET_LIBS}
    )

    nls_add_meta_generation(${TARGET_NAME} "${TARGET_SOURCE_DIR}" "${HEAD_FILES}")
endfunction()

function(nls_add_meta_generation TARGET_NAME SOURCE_DIR HEADERS)
    set(_target_name "${TARGET_NAME}")
    set(_source_dir "${SOURCE_DIR}")
    set(_headers ${HEADERS} ${ARGN})

    if(NOT TARGET MetaParser)
        message(FATAL_ERROR "MetaParser target must exist before runtime targets are configured.")
    endif()

    set(_gen_dir "${_source_dir}/Gen")
    set(_gen_cpp "${_gen_dir}/MetaGenerated.cpp")
    set(_gen_h "${_gen_dir}/MetaGenerated.h")
    set(_params_template "${NLS_ROOT_DIR}/Tools/MetaParser/precompile.json.in")
    set(_params_file "${CMAKE_CURRENT_BINARY_DIR}/${_target_name}.precompile.json")

    nls_get_common_meta_include_dirs(_common_include_dirs)
    set(_include_dirs ${_common_include_dirs} ${_source_dir} ${TARGET_APPEND_INCLUDE_DIR})

    nls_choose_meta_parser_compiler(_meta_parser_compiler _compiler_target _system_include_dirs)
    nls_detect_compiler_builtin_defines("${_meta_parser_compiler}" _builtin_defines)

    if(UNIX AND "${_meta_parser_compiler}" STREQUAL "/usr/bin/g++-9")
        list(APPEND _builtin_defines
            __GCC_ATOMIC_BOOL_LOCK_FREE=2
            __GCC_ATOMIC_CHAR16_T_LOCK_FREE=2
            __GCC_ATOMIC_CHAR32_T_LOCK_FREE=2
            __GCC_ATOMIC_CHAR8_T_LOCK_FREE=2
            __GCC_ATOMIC_CHAR_LOCK_FREE=2
            __GCC_ATOMIC_INT_LOCK_FREE=2
            __GCC_ATOMIC_LLONG_LOCK_FREE=2
            __GCC_ATOMIC_LONG_LOCK_FREE=2
            __GCC_ATOMIC_POINTER_LOCK_FREE=2
            __GCC_ATOMIC_SHORT_LOCK_FREE=2
            __GCC_ATOMIC_TEST_AND_SET_TRUEVAL=1
            __GCC_ATOMIC_WCHAR_T_LOCK_FREE=2
        )
    endif()

    set(_defines __REFLECTION_PARSER__)
    list(APPEND _defines ${_builtin_defines})

    get_target_property(_target_defines ${_target_name} COMPILE_DEFINITIONS)
    if(_target_defines)
        list(APPEND _defines ${_target_defines})
    endif()

    if(NLS_USE_SHARED_LIBRARY)
        string(TOUPPER ${_target_name} _export_macro)
        list(APPEND _defines "${_export_macro}_EXPORT")
    endif()

    if("${_meta_parser_compiler}" MATCHES "g..-9$")
        set(_compiler_options -std=c++2a)
    else()
        set(_compiler_options -std=c++20)
    endif()
    if(APPLE AND "${_meta_parser_compiler}" MATCHES "clang\\+\\+|clang$")
        list(APPEND _compiler_options -stdlib=libc++)
    endif()
    if(UNIX AND NOT APPLE AND "${_meta_parser_compiler}" MATCHES "clang\\+\\+|clang$")
        list(APPEND _compiler_options --gcc-toolchain=/usr)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR "${_meta_parser_compiler}" MATCHES "clang\\+\\+|clang$")
        list(APPEND _compiler_options -Wno-pragma-once-outside-header)
    endif()

    set(_resource_dir "")
    set(_sysroot "")
    nls_detect_clang_resource_dir(_resource_dir COMPILER "${_meta_parser_compiler}")
    nls_detect_apple_sysroot(_sysroot)

    list(FILTER _headers EXCLUDE REGEX "/Gen/")
    list(FILTER _headers EXCLUDE REGEX "/Generated/")
    list(FILTER _headers EXCLUDE REGEX "/Reflection/Macros\.h$")
    list(REMOVE_DUPLICATES _headers)
    list(REMOVE_DUPLICATES _include_dirs)
    list(REMOVE_DUPLICATES _system_include_dirs)
    list(REMOVE_DUPLICATES _defines)
    list(REMOVE_DUPLICATES _compiler_options)

    nls_join_json_array(NLS_META_HEADERS_JSON ${_headers})
    nls_join_json_array(NLS_META_INCLUDE_DIRS_JSON ${_include_dirs})
    nls_join_json_array(NLS_META_SYSTEM_INCLUDE_DIRS_JSON ${_system_include_dirs})
    nls_join_json_array(NLS_META_DEFINES_JSON ${_defines})
    nls_join_json_array(NLS_META_COMPILER_OPTIONS_JSON ${_compiler_options})
    nls_json_quote(NLS_META_ROOT_DIR "${NLS_ROOT_DIR}")
    nls_json_quote(NLS_META_TARGET_NAME "${_target_name}")
    nls_json_quote(NLS_META_MODULE_NAME "${_target_name}")
    nls_json_quote(NLS_META_OUTPUT_DIR "${_gen_dir}")
    nls_json_quote(NLS_META_COMPILER_PATH "${_meta_parser_compiler}")
    nls_json_quote(NLS_META_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
    nls_json_quote(NLS_META_COMPILER_TARGET "${_compiler_target}")
    nls_json_quote(NLS_META_RESOURCE_DIR "${_resource_dir}")
    nls_json_quote(NLS_META_SYSROOT "${_sysroot}")

    configure_file("${_params_template}" "${_params_file}" @ONLY)

    set(_meta_parser_env_args "")
    set(_meta_parser_native_dirs "")
    set(_nuget_root "$ENV{HOME}/.nuget/packages")

    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(_nuget_runtime_id "win-arm64")
        else()
            set(_nuget_runtime_id "win-x64")
        endif()
        list(APPEND _meta_parser_native_dirs
            "${_nuget_root}/libclang.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANG_VERSION}/runtimes/${_nuget_runtime_id}/native"
            "${_nuget_root}/libclangsharp.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANGSHARP_VERSION}/runtimes/${_nuget_runtime_id}/native")
        set(_meta_parser_path "$ENV{PATH}")
        foreach(_native_dir IN LISTS _meta_parser_native_dirs)
            if(EXISTS "${_native_dir}")
                if(_meta_parser_path)
                    set(_meta_parser_path "${_native_dir};${_meta_parser_path}")
                else()
                    set(_meta_parser_path "${_native_dir}")
                endif()
            endif()
        endforeach()
        if(_meta_parser_path)
            list(APPEND _meta_parser_env_args "PATH=${_meta_parser_path}")
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_nuget_runtime_id "osx-arm64")
        else()
            set(_nuget_runtime_id "osx-x64")
        endif()
        list(APPEND _meta_parser_native_dirs
            "${_nuget_root}/libclang.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANG_OSX_VERSION}/runtimes/${_nuget_runtime_id}/native"
            "${_nuget_root}/libclangsharp.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANGSHARP_VERSION}/runtimes/${_nuget_runtime_id}/native")
        set(_meta_parser_dyld "$ENV{DYLD_LIBRARY_PATH}")
        foreach(_native_dir IN LISTS _meta_parser_native_dirs)
            if(EXISTS "${_native_dir}")
                if(_meta_parser_dyld)
                    set(_meta_parser_dyld "${_native_dir}:${_meta_parser_dyld}")
                else()
                    set(_meta_parser_dyld "${_native_dir}")
                endif()
            endif()
        endforeach()
        if(_meta_parser_dyld)
            list(APPEND _meta_parser_env_args "DYLD_LIBRARY_PATH=${_meta_parser_dyld}")
        endif()
    elseif(UNIX)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(_nuget_runtime_id "linux-arm64")
        else()
            set(_nuget_runtime_id "linux-x64")
        endif()
        list(APPEND _meta_parser_native_dirs
            "${_nuget_root}/libclang.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANG_VERSION}/runtimes/${_nuget_runtime_id}/native"
            "${_nuget_root}/libclangsharp.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANGSHARP_VERSION}/runtimes/${_nuget_runtime_id}/native")
        set(_meta_parser_ld "$ENV{LD_LIBRARY_PATH}")
        foreach(_native_dir IN LISTS _meta_parser_native_dirs)
            if(EXISTS "${_native_dir}")
                if(_meta_parser_ld)
                    set(_meta_parser_ld "${_native_dir}:${_meta_parser_ld}")
                else()
                    set(_meta_parser_ld "${_native_dir}")
                endif()
            endif()
        endforeach()
        if(_meta_parser_ld)
            list(APPEND _meta_parser_env_args "LD_LIBRARY_PATH=${_meta_parser_ld}")
        endif()
    endif()

    add_custom_command(
        OUTPUT "${_gen_cpp}" "${_gen_h}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen_dir}"
        COMMAND ${CMAKE_COMMAND} -E env ${_meta_parser_env_args} "${NLS_META_PARSER_EXECUTABLE}" "${_params_file}"
        DEPENDS
            MetaParser
            ${_headers}
            "${_params_template}"
            "${NLS_ROOT_DIR}/Tools/MetaParser/src/Main.cs"
            "${NLS_ROOT_DIR}/Tools/MetaParser/src/Templates/MetaGenerated.cpp.tt"
        VERBATIM
        COMMENT "Generating reflection bindings for ${_target_name}"
    )

    add_dependencies(${_target_name} MetaParser)
    target_include_directories(${_target_name} PRIVATE "${_gen_dir}")
    target_sources(${_target_name} PRIVATE "${_gen_cpp}" "${_gen_h}")
endfunction()

add_subdirectory("Engine")
add_subdirectory("Math")
add_subdirectory("Platform")
add_subdirectory("Core")
add_subdirectory("Base")
add_subdirectory("Rendering")
add_subdirectory("UI")
