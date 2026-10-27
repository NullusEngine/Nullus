# Packaging pipeline shared by the editor, NullusBuild, and CI.
#
# NLS_Build deliberately does not use reflection: it is a build-time library
# consumed by tools, and keeping it free of generated sources keeps the
# packaging tool independent from editor code generation.
nls_add_runtime_target(
  NAME NLS_Build
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  NO_REFLECTION
  LIBS
    NLS_Core
    NLS_Engine
    NLS_Cook
    zstd
  )

target_compile_definitions(NLS_Build PRIVATE
  NLS_ENGINE_VERSION_STRING="${PROJECT_VERSION}")

# Deterministic ZIP output uses zlib when the bundled copy is available; the
# writer falls back to stored entries otherwise.
if(TARGET zlibstatic)
  target_link_libraries(NLS_Build PRIVATE zlibstatic)
  target_include_directories(NLS_Build PRIVATE
    "${NLS_ThirdParty_DIR}/assimp/contrib/zlib"
    "${CMAKE_BINARY_DIR}/ThirdParty/assimp/contrib/zlib")
  target_compile_definitions(NLS_Build PRIVATE NLS_BUILD_HAS_ZLIB=1)
endif()

if(WIN32)
  target_link_libraries(NLS_Build PRIVATE shell32)
endif()

set_target_properties(NLS_Build PROPERTIES FOLDER Runtime)
