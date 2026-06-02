function(nls_split_project_entry_sources OUT_ENTRY_SOURCE OUT_LIBRARY_SOURCES)
    set(_entry_sources)
    set(_library_sources)

    foreach(_source IN LISTS ARGN)
        set(_is_entry_source FALSE)
        if(_source MATCHES "\\.(c|cc|cpp|cxx)$")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}")
            file(STRINGS "${_source}" _main_matches
                REGEX "^[ \t]*(int|auto)[ \t]+([A-Za-z_][A-Za-z0-9_]*[ \t]+)*(main|wWinMain|WinMain)[ \t]*\\("
                LIMIT_COUNT 1
            )
            if(_main_matches)
                set(_is_entry_source TRUE)
            endif()
        endif()

        if(_is_entry_source)
            list(APPEND _entry_sources "${_source}")
        else()
            list(APPEND _library_sources "${_source}")
        endif()
    endforeach()

    list(LENGTH _entry_sources _entry_source_count)
    if(NOT _entry_source_count EQUAL 1)
        message(FATAL_ERROR "Expected exactly one executable entry source, found ${_entry_source_count}.")
    endif()

    set(${OUT_ENTRY_SOURCE} ${_entry_sources} PARENT_SCOPE)
    set(${OUT_LIBRARY_SOURCES} ${_library_sources} PARENT_SCOPE)
endfunction()

add_subdirectory("Launcher")
set_target_properties(Launcher PROPERTIES FOLDER "Project")
add_subdirectory("Game")
set_target_properties(Game PROPERTIES FOLDER "Project")
add_subdirectory("Editor")
set_target_properties(Editor PROPERTIES FOLDER "Project")
