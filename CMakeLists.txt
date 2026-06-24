cmake_minimum_required(VERSION 3.20)
project(Mosaicraft VERSION 0.2.0 LANGUAGES CXX)

# C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options("/utf-8")
endif()

find_package(OpenCV REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(TIFF REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(CUDAToolkit REQUIRED)

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

# ============================================================
# CUDA 后端（预编译 .obj）
# ============================================================
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

# ============================================================
# 可执行文件
# ============================================================
add_executable(mosaicraft
    src/main.cpp
)

target_link_libraries(mosaicraft PRIVATE
    mosaicraft_core
    mosaicraft_cuda
)
