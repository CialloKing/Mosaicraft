# Run from an x64 Visual Studio Developer PowerShell.
cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/smart-crop-cutlass-lock.cmake")
if(NOT WIN32 OR NOT DEFINED ENV{VCToolsInstallDir} OR NOT "$ENV{VSCMD_ARG_TGT_ARCH}" MATCHES "^(x64|amd64)$")
    message(FATAL_ERROR "Use an x64 Visual Studio Developer PowerShell")
endif()
if(NOT IS_ABSOLUTE "${CUDA_ROOT}" OR NOT IS_ABSOLUTE "${WORK}")
    message(FATAL_ERROR "Supply absolute -DCUDA_ROOT and -DWORK paths")
endif()
set(nvcc "${CUDA_ROOT}/bin/nvcc.exe")
execute_process(COMMAND "${nvcc}" --version OUTPUT_VARIABLE version COMMAND_ERROR_IS_FATAL ANY)
if(NOT version MATCHES "V13[.]5[.]37([ ,\r\n]|$)")
    message(FATAL_ERROR "Expected nvcc 13.5.37, got: ${version}")
endif()
set(name "cutlass-${MOSAICRAFT_CUTLASS_COMMIT}")
set(archive "${WORK}/${name}.zip")
file(MAKE_DIRECTORY "${WORK}")
file(DOWNLOAD "https://codeload.github.com/NVIDIA/cutlass/zip/${MOSAICRAFT_CUTLASS_COMMIT}" "${archive}"
    EXPECTED_HASH "SHA512=${MOSAICRAFT_CUTLASS_SHA512}" TLS_VERIFY ON)
file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${WORK}")
execute_process(COMMAND "${nvcc}" -std=c++17 -O3 -arch=sm_89 --expt-relaxed-constexpr
    -Xcompiler /Zc:preprocessor -Xcompiler /utf-8 -I "${WORK}/${name}/include"
    "${CMAKE_CURRENT_LIST_DIR}/smart_crop_cutlass_probe.cu" -o "${WORK}/smart_crop_cutlass_probe.exe"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${WORK}/smart_crop_cutlass_probe.exe" COMMAND_ERROR_IS_FATAL ANY)
