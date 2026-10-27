# Shared headless asset cooking.
#
# This target owns the parts of the editor import pipeline that do not need UI,
# so the editor, NullusBuild, and CI produce identical target artifacts.
nls_add_runtime_target(
  NAME NLS_Cook
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  NO_REFLECTION
  LIBS
    NLS_Core
    NLS_Engine
    NLS_Render
    NLS_StaticMeshBuild
    NLS_MaterialCompiler
  )

set_target_properties(NLS_Cook PROPERTIES FOLDER Runtime)

if(WIN32 AND TARGET DirectXTex::DirectXTex)
  target_link_libraries(NLS_Cook PRIVATE DirectXTex::DirectXTex)
  target_compile_definitions(NLS_Cook PRIVATE
    NLS_HAS_DIRECTXTEX=1
    NLS_DIRECTXTEX_VERSION="cook")
endif()
