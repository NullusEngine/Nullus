function(nls_add_package_zip root)
    if(TARGET NullusPackageZip)
        return()
    endif()
    if(NOT TARGET zlibstatic)
        add_subdirectory("${root}/ThirdParty/assimp/contrib/zlib" "${CMAKE_BINARY_DIR}/package-zlib" EXCLUDE_FROM_ALL)
    endif()
    add_library(NullusPackageZip STATIC
        "${root}/ThirdParty/assimp/contrib/unzip/unzip.c"
        "${root}/ThirdParty/assimp/contrib/unzip/ioapi.c")
    target_include_directories(NullusPackageZip PUBLIC
        "${root}/ThirdParty/assimp/contrib/unzip"
        "${root}/ThirdParty/assimp/contrib/zlib"
        "$<TARGET_PROPERTY:zlibstatic,BINARY_DIR>")
    target_compile_definitions(NullusPackageZip PRIVATE NOUNCRYPT)
    target_link_libraries(NullusPackageZip PUBLIC zlibstatic)
    set_target_properties(NullusPackageZip PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "ThirdParty")
endfunction()
