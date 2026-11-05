include("${NLS_ROOT_DIR}/Tools/CMake/NullusPackageZip.cmake")
nls_add_package_zip("${NLS_ROOT_DIR}")
nls_add_runtime_target(NAME NLS_Packages SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    NO_REFLECTION LIBS NLS_Plugins NLS_Core NLS_Platform NullusPackageZip)
include("${NLS_ROOT_DIR}/Tools/CMake/NullusPackageTools.cmake")
nls_configure_package_tools(NLS_Packages)
