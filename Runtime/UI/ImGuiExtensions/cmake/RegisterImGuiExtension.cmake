function(nls_register_imgui_extension)
  set(options)
  set(oneValueArgs NAME)
  set(multiValueArgs INCLUDE_DIRS COMPILE_DEFINITIONS COMPILE_OPTIONS LIBS)
  cmake_parse_arguments(NLS_IMGUI_EXTENSION "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT NLS_IMGUI_EXTENSION_NAME)
    message(FATAL_ERROR "nls_register_imgui_extension requires NAME")
  endif()

  if(NOT TARGET NLS_UI)
    message(FATAL_ERROR "nls_register_imgui_extension must be called after NLS_UI is created")
  endif()

  set(extension_root "${CMAKE_CURRENT_LIST_DIR}/${NLS_IMGUI_EXTENSION_NAME}")
  file(GLOB_RECURSE extension_files CONFIGURE_DEPENDS
    "${extension_root}/*.cpp"
    "${extension_root}/*.c"
    "${extension_root}/*.h"
    "${extension_root}/*.hpp"
    "${extension_root}/*.inl"
  )

  target_sources(NLS_UI PRIVATE ${extension_files})
  source_group(TREE "${extension_root}" PREFIX "ImGuiExtensions/${NLS_IMGUI_EXTENSION_NAME}" FILES ${extension_files})

  target_include_directories(NLS_UI PUBLIC
    $<BUILD_INTERFACE:${extension_root}>
  )

  foreach(include_dir IN LISTS NLS_IMGUI_EXTENSION_INCLUDE_DIRS)
    target_include_directories(NLS_UI PUBLIC
      $<BUILD_INTERFACE:${extension_root}/${include_dir}>
    )
  endforeach()

  if(NLS_IMGUI_EXTENSION_COMPILE_DEFINITIONS)
    target_compile_definitions(NLS_UI PRIVATE ${NLS_IMGUI_EXTENSION_COMPILE_DEFINITIONS})
  endif()

  if(NLS_IMGUI_EXTENSION_COMPILE_OPTIONS)
    target_compile_options(NLS_UI PRIVATE ${NLS_IMGUI_EXTENSION_COMPILE_OPTIONS})
  endif()

  if(NLS_IMGUI_EXTENSION_LIBS)
    target_link_libraries(NLS_UI PRIVATE ${NLS_IMGUI_EXTENSION_LIBS})
  endif()
endfunction()
