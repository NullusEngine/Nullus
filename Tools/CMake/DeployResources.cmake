# Several executables share one app directory. Serialize resource publication
# there and preserve timestamps so subsequent consumers skip unchanged files.
foreach(_required SOURCE_DIR DEST_DIR LOCK_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "DeployResources requires ${_required}")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${SOURCE_DIR}")
    message(FATAL_ERROR "Resource source directory does not exist: ${SOURCE_DIR}")
endif()
get_filename_component(_source "${SOURCE_DIR}" REALPATH)
get_filename_component(_destination "${DEST_DIR}" ABSOLUTE)
if(_source STREQUAL _destination)
    message(FATAL_ERROR "Resource source and destination must be different directories")
endif()
file(MAKE_DIRECTORY "${LOCK_DIR}")
file(LOCK "${LOCK_DIR}/.nullus-resource-deploy.lock" GUARD PROCESS TIMEOUT 180 RESULT_VARIABLE _lock_result)
if(NOT _lock_result STREQUAL "0")
    message(FATAL_ERROR "Could not lock resource destination ${LOCK_DIR}: ${_lock_result}")
endif()
file(MAKE_DIRECTORY "${DEST_DIR}")
file(COPY "${SOURCE_DIR}/" DESTINATION "${DEST_DIR}")
if(REMOVE_LEGACY_BRAND)
    file(REMOVE
        "${DEST_DIR}/Editor/Brand/NullusLogoMark.png"
        "${DEST_DIR}/Engine/Brand/NullusLogoMark.png"
        "${DEST_DIR}/Engine/Brand/NullusLogoMark.ico")
endif()
