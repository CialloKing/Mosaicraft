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
find_package(CUDAToolkit REQUIRED)

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
        -arch=sm_89 -std=c++17
        -ccbin "${CMAKE_CXX_COMPILER}"
        -I"${CMAKE_SOURCE_DIR}"
        -I"D:/C++project/SDK/vcpkg/installed/x64-windows/include/opencv4"
        -I"D:/C++project/SDK/vcpkg/installed/x64-windows/include"
        -Xcompiler "/utf-8" --compiler-options "/MD"
    DEPENDS "${CMAKE_SOURCE_DIR}/compute/CudaBackend.cu"
            "${CMAKE_SOURCE_DIR}/compute/CudaBackend.h"
    COMMENT "Compiling CudaBackend.cu..."
)

add_custom_command(
    OUTPUT ${CUDA_OBJ2}
    COMMAND "${CUDAToolkit_BIN_DIR}/nvcc.exe"
        -c "${CMAKE_SOURCE_DIR}/compute/FeatureExtractorCuda.cu"
        -o "${CUDA_OBJ2}"
        -arch=sm_89 -std=c++17
        -ccbin "${CMAKE_CXX_COMPILER}"
        -I"${CMAKE_SOURCE_DIR}"
        -I"D:/C++project/SDK/vcpkg/installed/x64-windows/include/opencv4"
        -I"D:/C++project/SDK/vcpkg/installed/x64-windows/include"
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
)

target_link_libraries(mosaicraft_core PUBLIC
    ${OpenCV_LIBS}
    unofficial::sqlite3::sqlite3
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
