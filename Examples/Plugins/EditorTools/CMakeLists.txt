add_library(NullusEditorToolsSample STATIC EditorTools.cpp)
target_compile_features(NullusEditorToolsSample PRIVATE cxx_std_20)
target_include_directories(NullusEditorToolsSample PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${NLS_ROOT_DIR}/ThirdParty")
target_link_libraries(NullusEditorToolsSample PUBLIC NLS_Plugins PRIVATE ImGui)
set_target_properties(NullusEditorToolsSample PROPERTIES FOLDER "Plugins/Examples")
set_target_properties(NullusEditorToolsSample PROPERTIES
    NLS_MODULE_HEADER "EditorTools.h" NLS_MODULE_FACTORY "NLS::Examples::CreateEditorTools")
