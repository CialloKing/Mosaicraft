cmake_minimum_required(VERSION 3.20)
project(Mosaicraft VERSION 0.2.0 LANGUAGES CXX)

# C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 输出目录：Release/bin/ (exe+dll)  Release/lib/ (.lib)
# $<CONFIG> 让 Debug/Release 各自独立，CMake 不会重复追加
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib)

if (MSVC)
    add_compile_options("/utf-8")
endif()

# --- 跨平台依赖 ---
# Windows (vcpkg): 保持默认
# Linux (apt):  1) unofficial-sqlite3 → SQLite3  2) CUDAToolkit REQUIRED → QUIET
#               3) nvcc.exe → nvcc  4) 删除 -Xcompiler "/utf-8" --compiler-options "/MD"
find_package(OpenCV REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(TIFF REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
# ============================================================
# CUDA 选项
# ============================================================
option(MOSAICRAFT_CUDA "Enable CUDA GPU acceleration (requires NVIDIA GPU + CUDA Toolkit)" ON)

if (MOSAICRAFT_CUDA)
    find_package(CUDAToolkit REQUIRED)
    message(STATUS "CUDA: enabled (${CUDAToolkit_VERSION})")
else()
    message(STATUS "CUDA: disabled (MOSAICRAFT_CUDA=OFF)")
endif()

# CUDA 架构（可通过 -DCUDA_ARCH=sm_xx 覆盖，默认 RTX 4060 / sm_89）
set(CUDA_ARCH "sm_89" CACHE STRING "Target CUDA compute capability")

# vcpkg include 路径，供 nvcc 使用（nvcc 不经过 target_include_directories）
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TRIPLET_INCLUDE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
else()
    message(WARNING "vcpkg toolchain not detected; nvcc may miss third-party headers")
    set(VCPKG_TRIPLET_INCLUDE "")
endif()

# ============================================================
# 手动编译 CUDA 源文件
# ============================================================
if (MOSAICRAFT_CUDA)
set(CUDA_OBJ1 "${CMAKE_BINARY_DIR}/CudaBackend.obj")
set(CUDA_OBJ2 "${CMAKE_BINARY_DIR}/FeatureExtractorCuda.obj")

add_custom_command(
    OUTPUT ${CUDA_OBJ1}
    COMMAND "${CUDAToolkit_BIN_DIR}/nvcc.exe"
        -c "${CMAKE_SOURCE_DIR}/compute/CudaBackend.cu"
        -o "${CUDA_OBJ1}"
        -arch=${CUDA_ARCH} -std=c++17
        -ccbin "${CMAKE_CXX_COMPILER}"
        -I"${CMAKE_SOURCE_DIR}"
        -I"${VCPKG_TRIPLET_INCLUDE}/opencv4"
        -I"${VCPKG_TRIPLET_INCLUDE}"
        -I"${TIFF_INCLUDE_DIR}"
        -Xcompiler "/utf-8" --compiler-options "/MD"
    DEPENDS "${CMAKE_SOURCE_DIR}/compute/CudaBackend.cu"
            "${CMAKE_SOURCE_DIR}/compute/CudaBackend.h"
    COMMENT "Compiling CudaBackend.cu (${CUDA_ARCH})..."
)

add_custom_command(
    OUTPUT ${CUDA_OBJ2}
    COMMAND "${CUDAToolkit_BIN_DIR}/nvcc.exe"
        -c "${CMAKE_SOURCE_DIR}/compute/FeatureExtractorCuda.cu"
        -o "${CUDA_OBJ2}"
        -arch=${CUDA_ARCH} -std=c++17
        -ccbin "${CMAKE_CXX_COMPILER}"
        -I"${CMAKE_SOURCE_DIR}"
        -I"${VCPKG_TRIPLET_INCLUDE}/opencv4"
        -I"${VCPKG_TRIPLET_INCLUDE}"
        -I"${TIFF_INCLUDE_DIR}"
        -Xcompiler "/utf-8" --compiler-options "/MD"
    DEPENDS "${CMAKE_SOURCE_DIR}/compute/FeatureExtractorCuda.cu"
            "${CMAKE_SOURCE_DIR}/compute/FeatureExtractorCuda.h"
    COMMENT "Compiling FeatureExtractorCuda.cu..."
)

endif()  # MOSAICRAFT_CUDA
# ============================================================
# 核心静态库（CPU）
# ============================================================
add_library(mosaicraft_core STATIC
    core/ImageNormalizer.cpp
    core/BatchProcessor.cpp
    core/Database.cpp
    core/FeatureExtractor.cpp
    core/MosaicEngine.cpp
)
if (NOT MOSAICRAFT_CUDA)
    target_sources(mosaicraft_core PRIVATE core/CudaStubs.cpp)
endif()

target_include_directories(mosaicraft_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TIFF_INCLUDE_DIR}
)

target_link_libraries(mosaicraft_core PUBLIC
    ${OpenCV_LIBS}
    unofficial::sqlite3::sqlite3
    ${TIFF_LIBRARIES}
    PNG::PNG
    JPEG::JPEG
)

if (MOSAICRAFT_CUDA)
add_library(mosaicraft_cuda STATIC
    ${CUDA_OBJ1}
    ${CUDA_OBJ2}
)
target_include_directories(mosaicraft_cuda PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mosaicraft_cuda PUBLIC
    CUDA::cudart
    mosaicraft_core
)
else()
add_library(mosaicraft_cuda INTERFACE)
target_link_libraries(mosaicraft_cuda INTERFACE mosaicraft_core)
endif()

# ============================================================
# 可执行文件
# ============================================================
add_executable(mosaicraft
    src/main.cpp
)

target_compile_definitions(mosaicraft PRIVATE
    $<$<BOOL:${MOSAICRAFT_CUDA}>:MOSAICRAFT_CUDA>
)

target_link_libraries(mosaicraft PRIVATE
    mosaicraft_core
    mosaicraft_cuda
)

# CUDA 运行时 DLL 自动部署（vcpkg 不管 CUDA DLL）
if (MOSAICRAFT_CUDA)
    file(GLOB CUDA_RUNTIME_DLL "${CUDAToolkit_BIN_DIR}/../bin/x64/cudart64_*.dll")
    if (CUDA_RUNTIME_DLL)
        list(GET CUDA_RUNTIME_DLL 0 CUDA_RUNTIME_DLL_FIRST)
        add_custom_command(TARGET mosaicraft POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CUDA_RUNTIME_DLL_FIRST}"
                "$<TARGET_FILE_DIR:mosaicraft>"
            COMMENT "Deploying CUDA runtime DLL..."
        )
    endif()
endif()

# ============================================================
# Web UI 服务器（独立进程，调用 mosaicraft.exe）
# ============================================================
add_executable(MosaicraftWebUI
    src/server.cpp
)
target_link_libraries(MosaicraftWebUI PRIVATE
    mosaicraft_core
)
target_include_directories(MosaicraftWebUI PRIVATE
    ${CMAKE_SOURCE_DIR}
)
# 发布时自动复制 HTML 到 exe 同目录
add_custom_command(TARGET MosaicraftWebUI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tools/command-builder/index.html"
        "$<TARGET_FILE_DIR:MosaicraftWebUI>/index.html"
)

# ============================================================
# 单元测试 (doctest — 单头文件，跨平台)
# ============================================================
add_executable(mosaicraft_tests EXCLUDE_FROM_ALL
    tests/test_core.cpp
)
target_link_libraries(mosaicraft_tests PRIVATE
    mosaicraft_core
)
target_include_directories(mosaicraft_tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)

# CTest 集成
enable_testing()
add_test(NAME mosaicraft_tests COMMAND mosaicraft_tests)
