add_library(NLS_MaterialCompiler STATIC MaterialShaderCompiler.cpp MaterialDependencySnapshot.cpp)
target_sources(NLS_MaterialCompiler PRIVATE MaterialGraph.cpp MaterialGraphCompiler.cpp MaterialCooker.cpp)
target_compile_features(NLS_MaterialCompiler PUBLIC cxx_std_20)
target_include_directories(NLS_MaterialCompiler PUBLIC "${NLS_ROOT_DIR}/Runtime")
target_link_libraries(NLS_MaterialCompiler PUBLIC NLS_Render)
set_target_properties(NLS_MaterialCompiler PROPERTIES
  FOLDER "Runtime/Build"
  POSITION_INDEPENDENT_CODE ON)
