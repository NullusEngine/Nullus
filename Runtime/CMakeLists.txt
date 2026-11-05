set(NLS_META_LIBCLANG_VERSION "18.1.3.2" CACHE INTERNAL "libclang runtime package version for MetaParser")
set(NLS_META_LIBCLANG_OSX_VERSION "18.1.3" CACHE INTERNAL "macOS libclang runtime package version for MetaParser")
set(NLS_META_LIBCLANGSHARP_VERSION "18.1.3.1" CACHE INTERNAL "libclangsharp runtime package version for MetaParser")

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
    list(FILTER _headers EXCLUDE REGEX "[/_\\\\]_DISABLED/")
    list(FILTER _sources EXCLUDE REGEX "[/_\\\\]_DISABLED/")

    set(_existing_headers "")
    foreach(_header IN LISTS _headers)
      if(EXISTS "${_header}")
        list(APPEND _existing_headers "${_header}")
      endif()
    endforeach()

    set(_existing_sources "")
    foreach(_source IN LISTS _sources)
      if(EXISTS "${_source}")
        list(APPEND _existing_sources "${_source}")
      endif()
    endforeach()

    set(${OUT_HEADERS} ${_existing_headers} PARENT_SCOPE)
    set(${OUT_SOURCES} ${_existing_sources} PARENT_SCOPE)
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

        set(_gnu_stdlib_includes "")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -dumpfullversion
            OUTPUT_VARIABLE _gcc_version
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT _gcc_version)
            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" -dumpversion
                OUTPUT_VARIABLE _gcc_version
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
        endif()
        string(REGEX MATCH "^[0-9]+" _gcc_major "${_gcc_version}")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-multiarch
            OUTPUT_VARIABLE _gcc_multiarch
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=include
            OUTPUT_VARIABLE _gcc_builtin_include
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        foreach(_dir
            "/usr/include/c++/${_gcc_major}"
            "/usr/include/${_gcc_multiarch}/c++/${_gcc_major}"
            "/usr/include/c++/${_gcc_major}/backward"
            "${_gcc_builtin_include}"
            "/usr/lib/gcc/${_gcc_multiarch}/${_gcc_major}/include")
            if(_dir)
                get_filename_component(_dir_real "${_dir}" REALPATH)
                if(_dir_real AND EXISTS "${_dir_real}")
                    list(APPEND _gnu_stdlib_includes "${_dir_real}")
                elseif(EXISTS "${_dir}")
                    list(APPEND _gnu_stdlib_includes "${_dir}")
                endif()
            endif()
        endforeach()
        if(_gnu_stdlib_includes)
            list(APPEND _gnu_stdlib_includes ${_includes})
            set(_includes ${_gnu_stdlib_includes})
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
                if(_line)
                    get_filename_component(_line_real "${_line}" REALPATH)
                    if(_line_real AND EXISTS "${_line_real}")
                        list(APPEND _includes "${_line_real}")
                    elseif(EXISTS "${_line}")
                        list(APPEND _includes "${_line}")
                    endif()
                endif()
            endforeach()
        endif()

        set(_needs_gnu_stdlib_paths FALSE)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(_needs_gnu_stdlib_paths TRUE)
            foreach(_include IN LISTS _includes)
                if(_include MATCHES "/include/c\\+\\+/[0-9]+")
                    set(_needs_gnu_stdlib_paths FALSE)
                    break()
                endif()
            endforeach()
        endif()

        if(_needs_gnu_stdlib_paths)
            set(_gcc_probe_compiler "${CMAKE_CXX_COMPILER}")
            if(NOT _gcc_probe_compiler)
                set(_gcc_probe_compiler "${COMPILER}")
            endif()
            execute_process(
                COMMAND "${_gcc_probe_compiler}" -dumpfullversion
                OUTPUT_VARIABLE _gcc_version
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(NOT _gcc_version)
                execute_process(
                    COMMAND "${_gcc_probe_compiler}" -dumpversion
                    OUTPUT_VARIABLE _gcc_version
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
            endif()
            string(REGEX MATCH "^[0-9]+" _gcc_major "${_gcc_version}")
            execute_process(
                COMMAND "${_gcc_probe_compiler}" -print-multiarch
                OUTPUT_VARIABLE _gcc_multiarch
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            execute_process(
                COMMAND "${_gcc_probe_compiler}" -print-file-name=include
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
                if(_dir)
                    get_filename_component(_dir_real "${_dir}" REALPATH)
                    if(_dir_real AND EXISTS "${_dir_real}")
                        list(APPEND _includes "${_dir_real}")
                    elseif(EXISTS "${_dir}")
                        list(APPEND _includes "${_dir}")
                    endif()
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
        # Scripting headers are visible to the reflection catalog even when
        # the current target is another runtime module (the catalog scans all
        # reflected Runtime headers for supported base types).
        "${NLS_ROOT_DIR}/Runtime/Scripting/Gen"
    )

    file(GLOB _runtime_children LIST_DIRECTORIES true "${NLS_ROOT_DIR}/Runtime/*")
    foreach(_dir ${_runtime_children})
        if(IS_DIRECTORY "${_dir}")
            list(APPEND _dirs "${_dir}")
            list(APPEND _dirs "${_dir}/Gen")
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

    nls_configure_shared_runtime_output(${TARGET_NAME})
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER ${TARGET_CATEGORY})

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
    set(options NO_REFLECTION)
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

    nls_configure_shared_runtime_output(${TARGET_NAME})
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER Runtime)

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

    nls_target_precompile_headers(${TARGET_NAME} "${NLS_ROOT_DIR}/Runtime/NullusRuntimePch.h")

    if(NOT TARGET_NO_REFLECTION)
        nls_add_meta_generation(${TARGET_NAME} "${TARGET_SOURCE_DIR}" "${HEAD_FILES}")
    endif()
endfunction()

function(nls_define_meta_generation_command)
    add_custom_command(
        OUTPUT
            "${_gen_stamp}"
            "${_gen_cpp}"
            "${_gen_h}"
            ${_meta_generated_side_outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen_dir}"
        COMMAND ${CMAKE_COMMAND} -E env ${_meta_parser_env_args} "${_meta_parser_command}" "${_params_file}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_gen_stamp}"
        DEPENDS
            ${_meta_parser_dependency_target}
            ${_headers}
            "${_params_file}"
            "${_params_template}"
            ${_meta_parser_tool_sources}
            ${_meta_parser_template_files}
        WORKING_DIRECTORY "${_meta_generation_working_directory}"
        VERBATIM
        COMMENT "Generating reflection bindings for ${_target_name}"
    )
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
    set(_gen_stamp "${CMAKE_CURRENT_BINARY_DIR}/${_target_name}_MetaGenerated.stamp")
    set(_params_template "${NLS_ROOT_DIR}/Tools/MetaParser/precompile.json.in")
    set(_params_file "${CMAKE_CURRENT_BINARY_DIR}/${_target_name}.precompile.json")
    file(GLOB _meta_parser_template_files CONFIGURE_DEPENDS
        "${NLS_ROOT_DIR}/Tools/MetaParser/src/Templates/*.tt"
    )
    file(GLOB_RECURSE _meta_parser_tool_sources CONFIGURE_DEPENDS
        "${NLS_ROOT_DIR}/Tools/MetaParser/src/*.cs"
    )
    list(FILTER _meta_parser_tool_sources EXCLUDE REGEX [[/bin/]])
    list(FILTER _meta_parser_tool_sources EXCLUDE REGEX [[/obj/]])

    nls_get_common_meta_include_dirs(_common_include_dirs)
    set(_include_dirs ${_common_include_dirs} ${_source_dir} ${_gen_dir} ${TARGET_APPEND_INCLUDE_DIR})

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
    if(WIN32)
        list(APPEND _defines _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    endif()

    get_target_property(_target_defines ${_target_name} COMPILE_DEFINITIONS)
    if(_target_defines)
        list(APPEND _defines ${_target_defines})
    endif()

    set(_visibility_defines "")
    if(NLS_USE_SHARED_LIBRARY)
        string(TOUPPER ${_target_name} _export_macro)
        list(APPEND _defines "${_export_macro}_EXPORT")
        list(APPEND _visibility_defines "${_export_macro}_EXPORT")
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

    # MetaParser emits one generated header/source pair per reflected header
    # in addition to the aggregate MetaGenerated.cpp/.h files. Register those
    # side outputs with the build graph so an interrupted or stale generation
    # cannot leave the compiler consuming a header stub while the old .cpp is
    # still considered up to date.
    set(_meta_generated_side_outputs "")
    foreach(_meta_header IN LISTS _headers)
        if(NOT EXISTS "${_meta_header}")
            continue()
        endif()

        file(READ "${_meta_header}" _meta_header_text)
        string(FIND "${_meta_header_text}" "CLASS(" _has_class)
        string(FIND "${_meta_header_text}" "STRUCT(" _has_struct)
        string(FIND "${_meta_header_text}" "ENUM(" _has_enum)
        if(_has_class LESS 0 AND _has_struct LESS 0 AND _has_enum LESS 0)
            continue()
        endif()

        # Match MetaParserTool.ToGeneratedIncludePath(): make the path
        # relative to Runtime and strip its first directory component. This
        # keeps Runtime/Base/Reflection/... under Gen/Reflection/... while
        # Project/Editor/... remains under Gen/Project/Editor/....
        file(RELATIVE_PATH _meta_relative_to_runtime
            "${NLS_ROOT_DIR}/Runtime"
            "${_meta_header}")
        string(REPLACE "\\" "/" _meta_relative_to_runtime "${_meta_relative_to_runtime}")
        string(FIND "${_meta_relative_to_runtime}" "/" _meta_first_slash)
        if(_meta_first_slash GREATER -1)
            math(EXPR _meta_generated_start "${_meta_first_slash} + 1")
            string(SUBSTRING "${_meta_relative_to_runtime}" "${_meta_generated_start}" -1 _meta_generated_relative)
        else()
            set(_meta_generated_relative "${_meta_relative_to_runtime}")
        endif()
        get_filename_component(_meta_generated_base "${_meta_generated_relative}" NAME_WE)
        get_filename_component(_meta_generated_dir "${_meta_generated_relative}" DIRECTORY)
        if(_meta_generated_dir STREQUAL "." OR _meta_generated_dir STREQUAL "")
            set(_meta_generated_relative_base "${_meta_generated_base}")
        else()
            set(_meta_generated_relative_base "${_meta_generated_dir}/${_meta_generated_base}")
        endif()

        list(APPEND _meta_generated_side_outputs
            "${_gen_dir}/${_meta_generated_relative_base}.generated.h"
            "${_gen_dir}/${_meta_generated_relative_base}.generated.cpp")
    endforeach()
    list(REMOVE_DUPLICATES _meta_generated_side_outputs)

    # The Engine manifest is also the project-level input consumed by the
    # managed generator. Keep both files in the same file-level build graph so
    # a clean configure cannot race Managed against reflection generation.
    if(_target_name STREQUAL "NLS_Engine")
        list(APPEND _meta_generated_side_outputs
            "${NLS_ROOT_DIR}/Library/ScriptApi/ScriptApi.json"
            "${NLS_ROOT_DIR}/Library/ScriptApi/ScriptApi.bin")
    endif()

    nls_join_json_array(NLS_META_HEADERS_JSON ${_headers})
    nls_join_json_array(NLS_META_INCLUDE_DIRS_JSON ${_include_dirs})
    nls_join_json_array(NLS_META_SYSTEM_INCLUDE_DIRS_JSON ${_system_include_dirs})
    nls_join_json_array(NLS_META_DEFINES_JSON ${_defines})
    nls_join_json_array(NLS_META_COMPILER_OPTIONS_JSON ${_compiler_options})
    nls_join_json_array(NLS_META_VISIBILITY_DEFINES_JSON ${_visibility_defines})
    if(NLS_META_PARSER_CACHE)
        set(NLS_META_CACHE_ENABLED true)
    else()
        set(NLS_META_CACHE_ENABLED false)
    endif()
    nls_json_quote(NLS_META_CACHE_DIRECTORY "${NLS_META_PARSER_CACHE_DIR}")
    nls_json_quote(NLS_META_ROOT_DIR "${NLS_ROOT_DIR}")
    nls_json_quote(NLS_META_SOURCE_DIR "${_source_dir}")
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
    set(_meta_parser_dependency_target MetaParser)
    set(_meta_parser_command "${NLS_META_PARSER_EXECUTABLE}")
    if(UNIX)
        get_property(_meta_parser_dotnet_executable CACHE NLS_META_PARSER_DOTNET PROPERTY VALUE)
        if(NOT _meta_parser_dotnet_executable)
            get_property(_meta_parser_dotnet_executable CACHE DOTNET_EXECUTABLE PROPERTY VALUE)
        endif()
        if(_meta_parser_dotnet_executable)
            get_filename_component(_meta_parser_dotnet_root "${_meta_parser_dotnet_executable}" DIRECTORY)
            list(APPEND _meta_parser_env_args "DOTNET_ROOT=${_meta_parser_dotnet_root}")
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
                list(APPEND _meta_parser_env_args "DOTNET_ROOT_ARM64=${_meta_parser_dotnet_root}")
            endif()
        endif()
    endif()
    # MetaParser and its native ClangSharp payload are restored into the
    # repository-local NuGet cache. Keep generation independent of the user's
    # global ~/.nuget/packages directory so offline builds use exactly the
    # checked-in package feed and extracted cache.
    if(NLS_META_PARSER_NUGET_PACKAGES)
        set(_nuget_root "${NLS_META_PARSER_NUGET_PACKAGES}")
    else()
        set(_nuget_root "${NLS_ROOT_DIR}/Tools/MetaParser/ThirdParty/.nuget/packages")
    endif()

    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(_nuget_runtime_id "win-arm64")
        else()
            set(_nuget_runtime_id "win-x64")
        endif()
        set(_meta_parser_path_prefix "")
        set(_meta_parser_libclang "${_nuget_root}/libclang.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANG_VERSION}/runtimes/${_nuget_runtime_id}/native/libclang.dll")
        set(_meta_parser_clangsharp "${_nuget_root}/libclangsharp.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANGSHARP_VERSION}/runtimes/${_nuget_runtime_id}/native/libClangSharp.dll")
        file(TO_NATIVE_PATH "${_meta_parser_libclang}" _meta_parser_libclang_native)
        file(TO_NATIVE_PATH "${_meta_parser_clangsharp}" _meta_parser_clangsharp_native)
        foreach(_native_dir
            "${_nuget_root}/libclang.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANG_VERSION}/runtimes/${_nuget_runtime_id}/native"
            "${_nuget_root}/libclangsharp.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANGSHARP_VERSION}/runtimes/${_nuget_runtime_id}/native")
            if(EXISTS "${_native_dir}")
                if(_meta_parser_path_prefix)
                    set(_meta_parser_path_prefix "${_meta_parser_path_prefix};${_native_dir}")
                else()
                    set(_meta_parser_path_prefix "${_native_dir}")
                endif()
            endif()
        endforeach()
        set(_meta_parser_launcher "${CMAKE_CURRENT_BINARY_DIR}/${_target_name}_run_metaparser_$<CONFIG>.cmd")
        file(GENERATE
            OUTPUT "${_meta_parser_launcher}"
            CONTENT "@echo off\nsetlocal\nset \"DOTNET_ROOT=${NLS_DOTNET_ROOT}\"\nset \"PATH=${_meta_parser_path_prefix};%PATH%\"\nif not exist \"${_meta_parser_libclang_native}\" echo [MetaParser] Missing native dependency: ${_meta_parser_libclang_native} & exit /b 5\nif not exist \"${_meta_parser_clangsharp_native}\" echo [MetaParser] Missing native dependency: ${_meta_parser_clangsharp_native} & exit /b 5\npowershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File \"${NLS_ROOT_DIR}/Tools/BuildCleanEnvironment.ps1\" \"$<SHELL_PATH:${NLS_META_PARSER_EXECUTABLE}>\" %*\nexit /b %ERRORLEVEL%\n"
        )
        set(_meta_parser_command "${_meta_parser_launcher}")
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_nuget_runtime_id "osx-arm64")
        else()
            set(_nuget_runtime_id "osx-x64")
        endif()
        set(_meta_parser_exe_dir "${CMAKE_BINARY_DIR}/Tools/MetaParser/publish/$<CONFIG>")
        set(_meta_parser_libclang "${_nuget_root}/libclang.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANG_OSX_VERSION}/runtimes/${_nuget_runtime_id}/native/libclang.dylib")
        set(_meta_parser_clangsharp "${_nuget_root}/libclangsharp.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANGSHARP_VERSION}/runtimes/${_nuget_runtime_id}/native/libClangSharp.dylib")
        list(APPEND _meta_parser_native_dirs
            "${_nuget_root}/libclang.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANG_OSX_VERSION}/runtimes/${_nuget_runtime_id}/native"
            "${_nuget_root}/libclangsharp.runtime.${_nuget_runtime_id}/${NLS_META_LIBCLANGSHARP_VERSION}/runtimes/${_nuget_runtime_id}/native")
        if(NOT TARGET MetaParserNativeDependencies)
            add_custom_target(MetaParserNativeDependencies
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_meta_parser_exe_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_meta_parser_libclang}" "${_meta_parser_exe_dir}/libclang.dylib"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_meta_parser_clangsharp}" "${_meta_parser_exe_dir}/libClangSharp.dylib"
                COMMAND /bin/ln -sfn "libClangSharp.dylib" "${_meta_parser_exe_dir}/libClangSharp.18.1.3.dylib"
                DEPENDS MetaParser
                VERBATIM
                COMMENT "Staging native dependencies for MetaParser"
            )
            set_target_properties(MetaParserNativeDependencies PROPERTIES FOLDER Tools)
        endif()
        set(_meta_parser_dependency_target MetaParserNativeDependencies)
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

    set_property(TARGET ${_target_name} PROPERTY NLS_META_BATCH_PARAMS "${_params_file}")
    set_property(TARGET ${_target_name} PROPERTY NLS_META_BATCH_STAMP "${_gen_stamp}")
    set_property(TARGET ${_target_name} PROPERTY NLS_META_BATCH_DEPENDENCY "${_meta_parser_dependency_target}")
    set_property(TARGET ${_target_name} PROPERTY NLS_META_BATCH_COMMAND "${_meta_parser_command}")
    set_property(TARGET ${_target_name} PROPERTY NLS_META_BATCH_ENVIRONMENT "${_meta_parser_env_args}")
    set_property(GLOBAL APPEND PROPERTY NLS_META_BATCH_TARGETS "${_target_name}")
    set(_meta_generation_working_directory "${CMAKE_CURRENT_BINARY_DIR}")
    if(NLS_ENABLE_PARALLEL_REFLECTION_GENERATION)
        # The generator depends on upstream generated headers, rather than
        # waiting for the upstream library's compile and link to finish. In VS
        # this needs a real generation project so the project scheduler can
        # overlap it with compilation. Keep it out of the runtime product folder.
        set(_generation_target "${_target_name}_Reflection")
        set(_generation_all_option ALL)
        get_target_property(_excluded ${_target_name} EXCLUDE_FROM_ALL)
        set(_generation_directory "${CMAKE_CURRENT_SOURCE_DIR}")
        while(_generation_directory AND NOT _excluded)
            get_property(_excluded DIRECTORY "${_generation_directory}" PROPERTY EXCLUDE_FROM_ALL)
            get_property(_generation_directory DIRECTORY "${_generation_directory}" PROPERTY PARENT_DIRECTORY)
        endwhile()
        if(_excluded)
            set(_generation_all_option "")
        endif()
        # Defining the command in its own directory gives exactly one VS
        # project ownership of the rule. The compiling project only consumes
        # the generated sources, preventing a second parser invocation.
        add_subdirectory("${NLS_ROOT_DIR}/Tools/CMake/ReflectionGeneration"
            "${CMAKE_CURRENT_BINARY_DIR}/ReflectionGeneration")
        set_property(TARGET ${_target_name} PROPERTY
            NLS_REFLECTION_GENERATION_TARGET "${_generation_target}")
        set_property(TARGET ${_target_name} PROPERTY
            NLS_REFLECTION_GENERATED_DIRECTORY "${_gen_dir}")
        set_property(TARGET ${_target_name} PROPERTY
            NLS_REFLECTION_GENERATED_STAMP "${_gen_stamp}")
        set_property(GLOBAL APPEND PROPERTY NLS_REFLECTION_MODULE_TARGETS "${_target_name}")
        add_dependencies(${_generation_target} MetaParser)
        add_dependencies(${_target_name} ${_generation_target})
    else()
        nls_define_meta_generation_command()
        add_dependencies(${_target_name} MetaParser)
    endif()
    set_source_files_properties("${_gen_cpp}" "${_gen_h}" PROPERTIES GENERATED TRUE)
    set_source_files_properties("${_gen_cpp}" PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
    target_include_directories(${_target_name} PUBLIC "${_gen_dir}")
    target_sources(${_target_name} PRIVATE "${_gen_cpp}" "${_gen_h}")
endfunction()

function(nls_finalize_reflection_generation)
    get_property(_modules GLOBAL PROPERTY NLS_REFLECTION_MODULE_TARGETS)
    set(_migration_records "")
    foreach(_module IN LISTS _modules)
        get_target_property(_generation_target ${_module} NLS_REFLECTION_GENERATION_TARGET)
        nls_collect_link_closure(${_module} _dependencies)
        foreach(_dependency IN LISTS _dependencies)
            if(_dependency STREQUAL _module)
                continue()
            endif()
            get_target_property(_dependency_generation ${_dependency} NLS_REFLECTION_GENERATION_TARGET)
            if(_dependency_generation)
                add_dependencies(${_generation_target} ${_dependency_generation})
            endif()
        endforeach()
        if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
            get_target_property(_binary ${_module} BINARY_DIR)
            get_target_property(_generated ${_module} NLS_REFLECTION_GENERATED_DIRECTORY)
            get_target_property(_stamp ${_module} NLS_REFLECTION_GENERATED_STAMP)
            set(_contract "")
            if(_module STREQUAL "NLS_Engine")
                set(_contract "${NLS_ROOT_DIR}/Library/ScriptApi")
            endif()
            string(APPEND _migration_records "${_module}|${_binary}|${_generated}|${_stamp}|${_contract}\n")
        endif()
    endforeach()
    if(_migration_records)
        # Existing parent .tlogs can still list the outputs now owned by the
        # generation project. Otherwise a later parent Clean deletes headers
        # that its generation dependency has just published.
        set(_migration_manifest "${CMAKE_BINARY_DIR}/reflection-tracking-migration.txt")
        file(WRITE "${_migration_manifest}" "${_migration_records}")
        execute_process(COMMAND powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass
            -File "${NLS_ROOT_DIR}/Tools/CMake/MigrateReflectionTracking.ps1"
            -Manifest "${_migration_manifest}"
            RESULT_VARIABLE _migration_result)
        if(NOT _migration_result EQUAL 0)
            message(FATAL_ERROR "Could not migrate reflection output tracking logs.")
        endif()
    endif()
endfunction()

add_subdirectory("Engine")
add_subdirectory("Cook")
add_subdirectory("Build")
add_subdirectory("Math")
add_subdirectory("Input")
add_subdirectory("Platform")
add_subdirectory("Core")
add_subdirectory("Plugins")
add_subdirectory("Packages")
add_subdirectory("Base")
add_subdirectory("ECS")
add_subdirectory("Rendering")
add_subdirectory("MaterialCompiler")
add_subdirectory("UI")
add_subdirectory("Scripting")
add_subdirectory("VFX")
add_subdirectory("Physics")
add_subdirectory("VFXCompiler")
add_subdirectory("Animation")
add_subdirectory("AnimationCompiler")
