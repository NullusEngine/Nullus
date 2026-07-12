cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS STAGE_DIR DEST_DIR EXPECTED_MANIFEST)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${STAGE_DIR}")
    message(FATAL_ERROR "Runtime stage directory does not exist: ${STAGE_DIR}")
endif()
if(NOT EXISTS "${EXPECTED_MANIFEST}")
    message(FATAL_ERROR "Expected runtime manifest does not exist: ${EXPECTED_MANIFEST}")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")
file(LOCK "${DEST_DIR}/.nullus-runtime-deploy.lock" GUARD PROCESS TIMEOUT 120)

if(NOT DEFINED CONFIG OR CONFIG STREQUAL "")
    set(manifest_config "NoConfig")
else()
    string(REGEX REPLACE "[^A-Za-z0-9_.-]" "_" manifest_config "${CONFIG}")
endif()
set(MANIFEST "${DEST_DIR}/.nullus-runtime-${manifest_config}.manifest")

set(previous_files)
if(EXISTS "${MANIFEST}")
    file(STRINGS "${MANIFEST}" previous_files)
endif()

file(STRINGS "${EXPECTED_MANIFEST}" expected_entries)
set(staged_files)
foreach(relative_path IN LISTS expected_entries)
    string(REPLACE "\\" "/" normalized_relative_path "${relative_path}")
    if(normalized_relative_path STREQUAL "" OR
       IS_ABSOLUTE "${normalized_relative_path}" OR
       normalized_relative_path MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "Invalid expected runtime manifest entry: ${relative_path}")
    endif()
    if(NOT EXISTS "${STAGE_DIR}/${normalized_relative_path}" OR
       IS_DIRECTORY "${STAGE_DIR}/${normalized_relative_path}")
        message(FATAL_ERROR "Expected runtime file is missing from staging: ${normalized_relative_path}")
    endif()
    list(APPEND staged_files "${normalized_relative_path}")
endforeach()
list(REMOVE_DUPLICATES staged_files)
list(SORT staged_files)

foreach(relative_path IN LISTS previous_files)
    string(REPLACE "\\" "/" normalized_relative_path "${relative_path}")
    if(relative_path STREQUAL "" OR
       IS_ABSOLUTE "${relative_path}" OR
       normalized_relative_path MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "Invalid runtime deployment manifest entry: ${relative_path}")
    endif()
    if(NOT relative_path IN_LIST staged_files)
        file(REMOVE "${DEST_DIR}/${relative_path}")
    endif()
endforeach()

foreach(relative_path IN LISTS staged_files)
    get_filename_component(relative_directory "${relative_path}" DIRECTORY)
    if(NOT relative_directory STREQUAL "")
        file(MAKE_DIRECTORY "${DEST_DIR}/${relative_directory}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${STAGE_DIR}/${relative_path}"
            "${DEST_DIR}/${relative_path}"
        RESULT_VARIABLE copy_result)
    if(NOT copy_result EQUAL 0)
        message(FATAL_ERROR "Failed to deploy runtime file: ${relative_path}")
    endif()
endforeach()

set(manifest_contents "")
foreach(relative_path IN LISTS staged_files)
    string(APPEND manifest_contents "${relative_path}\n")
endforeach()
file(WRITE "${MANIFEST}.tmp" "${manifest_contents}")
file(RENAME "${MANIFEST}.tmp" "${MANIFEST}")
