# Keep build-tree lookup separate from the relocatable archive layout.
if(APPLE)
    set(_package_rpath "@loader_path/lib")
elseif(UNIX)
    set(_package_rpath "$ORIGIN/lib")
endif()
set_target_properties(mosaicraft MosaicraftWebUI PROPERTIES INSTALL_RPATH "${_package_rpath}")
install(TARGETS mosaicraft MosaicraftWebUI RUNTIME DESTINATION .)
install(FILES "${CMAKE_SOURCE_DIR}/tools/command-builder/index.html" DESTINATION .)
# Ship the license texts associated with the locked dependency graph.
file(GLOB _dependency_licenses "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
foreach(_license IN LISTS _dependency_licenses)
    get_filename_component(_port_dir "${_license}" DIRECTORY)
    get_filename_component(_port "${_port_dir}" NAME)
    install(FILES "${_license}" DESTINATION "licenses/${_port}")
endforeach()

if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    include(InstallRequiredSystemLibraries)
    get_filename_component(_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    set(_runtime_scanner "${_linker_dir}/dumpbin.exe")
endif()
set(_runtime_dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" "${CUDAToolkit_LIBRARY_DIR}")
if(MOSAICRAFT_CUDA AND WIN32)
    list(APPEND _runtime_dirs "${CUDAToolkit_BIN_DIR}" "${CUDAToolkit_BIN_DIR}/x64")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/DeployRuntime.cmake.in"
    "${CMAKE_BINARY_DIR}/DeployRuntime.in.cmake" @ONLY)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/DeployRuntime-$<CONFIG>.cmake"
    INPUT "${CMAKE_BINARY_DIR}/DeployRuntime.in.cmake")
install(SCRIPT "${CMAKE_BINARY_DIR}/DeployRuntime-$<CONFIG>.cmake")
