option(NLS_ENABLE_EMBREE "Use Embree 4 for offline distance field queries when its SDK is available" ON)
set(NLS_EMBREE_ROOT "" CACHE PATH "Embree 4 SDK root (include, lib and bin)")
option(NLS_EMBREE_NEEDS_TBB "The Windows Embree SDK uses oneTBB (disable for an internal-tasking SDK)" ON)
set(NLS_DISTANCE_FIELD_HAS_EMBREE OFF)
if(NLS_ENABLE_EMBREE)
    find_path(NLS_EMBREE_INCLUDE_DIR embree4/rtcore.h
        HINTS "${NLS_EMBREE_ROOT}/include" "${CMAKE_BINARY_DIR}/_deps/embree-sdk/include")
    find_library(NLS_EMBREE_LIBRARY NAMES embree4
        HINTS "${NLS_EMBREE_ROOT}/lib" "${CMAKE_BINARY_DIR}/_deps/embree-sdk/lib")
    if(NLS_EMBREE_INCLUDE_DIR AND NLS_EMBREE_LIBRARY)
        if(WIN32)
            find_file(NLS_EMBREE_RUNTIME embree4.dll
                HINTS "${NLS_EMBREE_ROOT}/bin" "${CMAKE_BINARY_DIR}/_deps/embree-sdk/bin")
            find_file(NLS_EMBREE_TBB_RUNTIME tbb12.dll
                HINTS "${NLS_EMBREE_ROOT}/bin" "${CMAKE_BINARY_DIR}/_deps/embree-sdk/bin")
            find_library(NLS_EMBREE_TBB_LIBRARY NAMES tbb12
                HINTS "${NLS_EMBREE_ROOT}/lib" "${CMAKE_BINARY_DIR}/_deps/embree-sdk/lib")
            if(NLS_EMBREE_RUNTIME AND (NOT NLS_EMBREE_NEEDS_TBB OR (NLS_EMBREE_TBB_RUNTIME AND NLS_EMBREE_TBB_LIBRARY)))
                add_library(NLS_Embree SHARED IMPORTED GLOBAL)
                set_target_properties(NLS_Embree PROPERTIES IMPORTED_LOCATION "${NLS_EMBREE_RUNTIME}"
                    IMPORTED_IMPLIB "${NLS_EMBREE_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${NLS_EMBREE_INCLUDE_DIR}")
                add_custom_target(NLS_EmbreeRuntime
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${NLS_RUNTIME_OUTPUT_PATH}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NLS_EMBREE_RUNTIME}" "${NLS_RUNTIME_OUTPUT_PATH}/embree4.dll"
                    VERBATIM)
                set_target_properties(NLS_EmbreeRuntime PROPERTIES FOLDER ThirdParty/Embree)
                find_file(NLS_EMBREE_LICENSE_FILE NAMES LICENSE.txt LICENSE
                    HINTS "${NLS_EMBREE_ROOT}" "${CMAKE_BINARY_DIR}/_deps/embree-sdk")
                if(NLS_EMBREE_LICENSE_FILE)
                    add_custom_command(TARGET NLS_EmbreeRuntime POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NLS_EMBREE_LICENSE_FILE}" "${NLS_RUNTIME_OUTPUT_PATH}/Embree-LICENSE.txt" VERBATIM)
                    set_property(TARGET NLS_Embree PROPERTY NLS_RUNTIME_COMPANION_FILES Embree-LICENSE.txt)
                endif()
                if(NLS_EMBREE_NEEDS_TBB)
                    add_library(NLS_EmbreeTbb SHARED IMPORTED GLOBAL)
                    set_target_properties(NLS_EmbreeTbb PROPERTIES IMPORTED_LOCATION "${NLS_EMBREE_TBB_RUNTIME}"
                        IMPORTED_IMPLIB "${NLS_EMBREE_TBB_LIBRARY}")
                    target_link_libraries(NLS_Embree INTERFACE NLS_EmbreeTbb)
                    add_custom_command(TARGET NLS_EmbreeRuntime POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NLS_EMBREE_TBB_RUNTIME}" "${NLS_RUNTIME_OUTPUT_PATH}/tbb12.dll" VERBATIM)
                    set_property(GLOBAL APPEND PROPERTY NLS_SHARED_RUNTIME_TARGETS NLS_EmbreeTbb)
                    find_file(NLS_EMBREE_TBB_LICENSE_FILE NAMES oneTBB-LICENSE.txt
                        HINTS "${NLS_EMBREE_ROOT}" "${CMAKE_BINARY_DIR}/_deps/embree-sdk")
                    if(NLS_EMBREE_TBB_LICENSE_FILE)
                        add_custom_command(TARGET NLS_EmbreeRuntime POST_BUILD
                            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NLS_EMBREE_TBB_LICENSE_FILE}" "${NLS_RUNTIME_OUTPUT_PATH}/oneTBB-LICENSE.txt" VERBATIM)
                        set_property(TARGET NLS_EmbreeTbb PROPERTY NLS_RUNTIME_COMPANION_FILES oneTBB-LICENSE.txt)
                    endif()
                endif()
                add_dependencies(NLS_StaticMeshBuild NLS_EmbreeRuntime)
                # Include the DLL only in consumers that link the build library.
                set_property(GLOBAL APPEND PROPERTY NLS_SHARED_RUNTIME_TARGETS NLS_Embree)
                set(NLS_DISTANCE_FIELD_HAS_EMBREE ON)
            endif()
        else()
            add_library(NLS_Embree UNKNOWN IMPORTED GLOBAL)
            set_target_properties(NLS_Embree PROPERTIES IMPORTED_LOCATION "${NLS_EMBREE_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${NLS_EMBREE_INCLUDE_DIR}")
            set(NLS_DISTANCE_FIELD_HAS_EMBREE ON)
        endif()
    endif()
endif()
if(NLS_DISTANCE_FIELD_HAS_EMBREE)
    target_link_libraries(NLS_StaticMeshBuild PRIVATE NLS_Embree)
    target_compile_definitions(NLS_StaticMeshBuild PRIVATE NLS_HAS_EMBREE=1)
    message(STATUS "Distance field query backend: Embree 4")
else()
    target_compile_definitions(NLS_StaticMeshBuild PRIVATE NLS_HAS_EMBREE=0)
    message(STATUS "Distance field query backend: built-in SAH BVH")
endif()
