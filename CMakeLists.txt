cmake_minimum_required(VERSION 3.20)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/contracts.json" MOSAICRAFT_CONTRACTS_JSON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/contracts.json")
foreach(key IN ITEMS softwareVersion libraryVersion featureVersion apiVersion apiMajorVersion annVersion
        featurePackVersion colorOrderVersion journalVersion studySchemaVersion checkpointPathsVersion uiStateVersion)
    string(JSON ${key} GET "${MOSAICRAFT_CONTRACTS_JSON}" ${key})
endforeach()
project(Mosaicraft VERSION ${softwareVersion} LANGUAGES C CXX)
configure_file(cmake/Contracts.h.in generated/MosaicraftContracts.h @ONLY)
set(previewScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/preview.js")
set(previewPickerScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/preview-picker.js")
set(resultAnalysisScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/result-analysis.js")
set(outputPlanningScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/output-planning.js")
set(libraryVisualsScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/library-visuals.js")
set(previewInsightsScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/preview-insights.js")
set(taskMetricsScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/task-metrics.js")
set(comparisonScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/comparison.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${previewScript}" "${previewPickerScript}" "${resultAnalysisScript}" "${outputPlanningScript}" "${libraryVisualsScript}" "${previewInsightsScript}" "${taskMetricsScript}" "${comparisonScript}")
file(READ "${previewScript}" interactivePreviewScript)
file(READ "${previewPickerScript}" interactivePreviewPickerScript)
file(READ "${resultAnalysisScript}" taskResultAnalysisScript)
file(READ "${outputPlanningScript}" mosaicOutputPlanningScript)
file(READ "${libraryVisualsScript}" libraryVisualsUiScript)
file(READ "${previewInsightsScript}" mosaicPreviewInsightsScript)
file(READ "${taskMetricsScript}" taskMetricsUiScript)
file(READ "${comparisonScript}" comparisonUiScript)
set(viewportScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/viewer-core.js")
set(portableScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/portable-viewer.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${viewportScript}" "${portableScript}")
file(READ "${viewportScript}" mosaicViewportScript)
file(READ "${portableScript}" portableViewerScript)
configure_file(cmake/PortableViewer.h.in generated/PortableViewer.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cli-options.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cli-options.json" mosaicCliOptionsJson)
configure_file(cmake/CliOptionsData.h.in generated/CliOptionsData.h @ONLY)
configure_file(tools/command-builder/index.html generated/index.html @ONLY)

# C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE AND VCPKG_TARGET_TRIPLET)
    # Match the pinned codec headers rather than the runner's Homebrew headers.
    set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
endif()

# 输出目录：Release/bin/ (exe+dll)  Release/lib/ (.lib)
# $<CONFIG> 让 Debug/Release 各自独立，CMake 不会重复追加
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib)

if (MSVC)
    add_compile_options("/utf-8")
endif()

# Homebrew 的 OpenCV 4 是版本化公式，默认不链接到 Homebrew 的公共前缀。
if(APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND NOT OpenCV_DIR)
    find_program(MOSAICRAFT_BREW brew)
    if(MOSAICRAFT_BREW)
        execute_process(COMMAND "${MOSAICRAFT_BREW}" --prefix opencv@4
            RESULT_VARIABLE _brew_result OUTPUT_VARIABLE _opencv_brew_prefix
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT _brew_result EQUAL 0)
            unset(_opencv_brew_prefix)
        endif()
    endif()
endif()
find_package(OpenCV 4 REQUIRED CONFIG COMPONENTS core imgproc imgcodecs HINTS "${_opencv_brew_prefix}")
# sqlite3：优先 vcpkg (unofficial-sqlite3)，回退到系统包 (SQLite3)
find_package(unofficial-sqlite3 CONFIG QUIET)
if(NOT unofficial-sqlite3_FOUND)
    find_package(SQLite3 REQUIRED)
endif()
find_package(TIFF REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(Threads REQUIRED)
# ============================================================
# CUDA 选项
# ============================================================
option(MOSAICRAFT_CUDA "Enable CUDA GPU acceleration (requires NVIDIA GPU + CUDA Toolkit)" "${WIN32}")

if (MOSAICRAFT_CUDA)
    include(cmake/CudaToolkit.cmake)
else()
    message(STATUS "CUDA: disabled (MOSAICRAFT_CUDA=OFF)")
endif()

# 单目标调用保持兼容；默认通用包同时携带七种原生内核及对应 PTX。
set(CUDA_ARCH "sm_75;sm_80;sm_86;sm_89;sm_90;sm_100;sm_120" CACHE STRING "Semicolon-separated native CUDA targets")
if(MOSAICRAFT_CUDA)
    include(cmake/CudaArchitectures.cmake)
    foreach(kind IN ITEMS code arch)
        execute_process(COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}" --list-gpu-${kind}
            RESULT_VARIABLE result OUTPUT_VARIABLE supported_${kind} ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Cannot query nvcc targets: ${error}")
        endif()
        string(REGEX MATCHALL "(sm|compute)_[0-9]+" supported_${kind} "${supported_${kind}}")
    endforeach()
    mosaicraft_cuda_architectures("${CUDA_ARCH}" "${supported_code}" "${supported_arch}"
        CUDA_NATIVE_TARGETS CUDA_PTX_TARGET CUDA_CODE_FLAGS)
    message(STATUS "CUDA native: ${CUDA_NATIVE_TARGETS}; PTX: ${CUDA_PTX_TARGET}")
    # configure_file semantics preserve the timestamp until targets/compiler change.
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/cuda-architectures.txt"
        CONTENT "CudaArchitectures: ${CUDA_NATIVE_TARGETS}\nCudaPtx: ${CUDA_PTX_TARGET}\nNvcc: ${CUDAToolkit_NVCC_EXECUTABLE}\nCudaToolkit: ${CUDAToolkit_VERSION}\nCudaRuntimeMode: ${CUDA_RUNTIME_MODE}\n")
endif()

# nvcc 平台适配：MSVC 需要额外的编译选项，对象文件扩展名也不同
if(MSVC)
    set(NVCC_EXTRA_FLAGS -Xcompiler;/utf-8;--compiler-options;/MD)
    set(CUDA_OBJ_EXT ".obj")
else()
    set(NVCC_EXTRA_FLAGS "")
    set(CUDA_OBJ_EXT ".o")
endif()

# ============================================================
# 手动编译 CUDA 源文件
# ============================================================
if (MOSAICRAFT_CUDA)
# 手动调用 nvcc 不会继承 CMake 目标的头文件路径，直接复用找到的 OpenCV。
set(NVCC_INCLUDE_FLAGS "")
foreach(_include IN LISTS OpenCV_INCLUDE_DIRS)
    list(APPEND NVCC_INCLUDE_FLAGS "-I${_include}")
endforeach()
set(CUDA_OBJ1 "${CMAKE_BINARY_DIR}/CudaBackend${CUDA_OBJ_EXT}")
set(CUDA_OBJ2 "${CMAKE_BINARY_DIR}/FeatureExtractorCuda${CUDA_OBJ_EXT}")
set(CUDA_OBJ3 "${CMAKE_BINARY_DIR}/CompatibleFeatures${CUDA_OBJ_EXT}")
set(CUDA_OBJ4 "${CMAKE_BINARY_DIR}/CompatibleScoring${CUDA_OBJ_EXT}")

add_custom_command(
    OUTPUT ${CUDA_OBJ1}
    COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}"
        -c "${CMAKE_SOURCE_DIR}/compute/CudaBackend.cu"
        -o "${CUDA_OBJ1}"
        ${CUDA_CODE_FLAGS} -std=c++17
        -ccbin "${CMAKE_CXX_COMPILER}"
        -I"${CMAKE_SOURCE_DIR}" -I"${CMAKE_BINARY_DIR}/generated"
        ${NVCC_INCLUDE_FLAGS}
        ${NVCC_EXTRA_FLAGS}
    VERBATIM
    DEPENDS "${CMAKE_BINARY_DIR}/cuda-architectures.txt" "${CMAKE_SOURCE_DIR}/compute/CudaBackend.cu" "${CMAKE_SOURCE_DIR}/compute/CudaBackend.h" "${CMAKE_SOURCE_DIR}/compute/DeviceBuffer.cuh" "${CMAKE_SOURCE_DIR}/compute/CudaPreflight.cuh"
    COMMENT "Compiling CudaBackend.cu (${CUDA_ARCH})..."
)

add_custom_command(
    OUTPUT ${CUDA_OBJ2}
    COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}"
        -c "${CMAKE_SOURCE_DIR}/compute/FeatureExtractorCuda.cu"
        -o "${CUDA_OBJ2}"
        ${CUDA_CODE_FLAGS} -std=c++17
        -ccbin "${CMAKE_CXX_COMPILER}"
        -I"${CMAKE_SOURCE_DIR}" -I"${CMAKE_BINARY_DIR}/generated"
        ${NVCC_INCLUDE_FLAGS}
        ${NVCC_EXTRA_FLAGS}
    VERBATIM
    DEPENDS "${CMAKE_BINARY_DIR}/cuda-architectures.txt" "${CMAKE_SOURCE_DIR}/compute/FeatureExtractorCuda.cu" "${CMAKE_SOURCE_DIR}/compute/FeatureExtractorCuda.h" "${CMAKE_SOURCE_DIR}/compute/CudaBackend.h" "${CMAKE_SOURCE_DIR}/compute/DeviceBuffer.cuh" "${CMAKE_SOURCE_DIR}/compute/CudaPreflight.cuh"
        "${CMAKE_SOURCE_DIR}/compute/TemplateFeatureSizes.h"
        "${CMAKE_SOURCE_DIR}/core/FeatureUtils.h" "${CMAKE_SOURCE_DIR}/core/GridWeights.inc" "${CMAKE_SOURCE_DIR}/core/FeatureExtractor.h" "${CMAKE_SOURCE_DIR}/core/UnicodeIO.h"
        "${CMAKE_SOURCE_DIR}/core/WorkerPool.h"
    COMMENT "Compiling FeatureExtractorCuda.cu..."
)

add_custom_command(
    OUTPUT ${CUDA_OBJ3}
    COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}"
        -c "${CMAKE_SOURCE_DIR}/compute/CompatibleFeatures.cu"
        -o "${CUDA_OBJ3}"
        ${CUDA_CODE_FLAGS} -std=c++17
        -ccbin "${CMAKE_CXX_COMPILER}"
        -I"${CMAKE_SOURCE_DIR}" -I"${CMAKE_BINARY_DIR}/generated"
        ${NVCC_INCLUDE_FLAGS}
        ${NVCC_EXTRA_FLAGS}
    VERBATIM
    DEPENDS "${CMAKE_BINARY_DIR}/cuda-architectures.txt" "${CMAKE_SOURCE_DIR}/compute/CompatibleFeatures.cu" "${CMAKE_SOURCE_DIR}/compute/CompatibleFeatures.h"
        "${CMAKE_SOURCE_DIR}/compute/CudaBackend.h" "${CMAKE_SOURCE_DIR}/compute/DeviceBuffer.cuh" "${CMAKE_SOURCE_DIR}/compute/CudaPreflight.cuh"
        "${CMAKE_SOURCE_DIR}/core/FeatureUtils.h" "${CMAKE_SOURCE_DIR}/core/GridWeights.inc" "${CMAKE_SOURCE_DIR}/core/FeatureExtractor.h"
        "${CMAKE_SOURCE_DIR}/core/WorkerPool.h"
    COMMENT "Compiling CPU-compatible dynamic CUDA features..."
)
# 只约束新评分内核；原模板的编译选项与算术保持不变。
add_custom_command(
    OUTPUT ${CUDA_OBJ4}
    COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}"
        -c "${CMAKE_SOURCE_DIR}/compute/CompatibleScoring.cu"
        -o "${CUDA_OBJ4}"
        ${CUDA_CODE_FLAGS} -std=c++17 --fmad=false --prec-div=true --prec-sqrt=true --ftz=false
        -ccbin "${CMAKE_CXX_COMPILER}"
        -I"${CMAKE_SOURCE_DIR}" -I"${CMAKE_BINARY_DIR}/generated"
        ${NVCC_INCLUDE_FLAGS}
        ${NVCC_EXTRA_FLAGS}
    VERBATIM
    DEPENDS "${CMAKE_BINARY_DIR}/cuda-architectures.txt" "${CMAKE_SOURCE_DIR}/compute/CompatibleScoring.cu" "${CMAKE_SOURCE_DIR}/compute/CompatibleScoring.h"
        "${CMAKE_SOURCE_DIR}/compute/CudaBackend.h" "${CMAKE_SOURCE_DIR}/compute/DeviceBuffer.cuh" "${CMAKE_SOURCE_DIR}/compute/CudaPreflight.cuh"
        "${CMAKE_SOURCE_DIR}/core/MatchingBatch.h" "${CMAKE_SOURCE_DIR}/core/GridWeights.inc"
    COMMENT "Compiling CPU-compatible CUDA scoring..."
)
endif()  # MOSAICRAFT_CUDA
# ============================================================
# 核心静态库（CPU）
# ============================================================
add_library(mosaicraft_archive STATIC third_party/miniz/miniz.c third_party/miniz/miniz_zip.c
    third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c)
target_include_directories(mosaicraft_archive PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(mosaicraft_archive PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

add_library(mosaicraft_core STATIC
    core/OutputPublication.cpp
    core/ProjectService.cpp
    core/RuntimeContext.cpp
    core/TaskResources.cpp
    core/HistoryStore.cpp
    core/AnalysisReport.cpp
    core/ApiHandlers.cpp
    core/ApiJson.cpp
    core/ApiMetadata.cpp
    core/ApiRequestParser.cpp
    core/ImageNormalizer.cpp
    core/BatchProcessor.cpp
    core/NormalizeService.cpp
    core/BuildService.cpp
    core/ColorOrder.cpp
    core/Database.cpp
    core/DatabaseService.cpp
    core/FeatureExtractor.cpp
    core/FeaturePack.cpp
    core/ExecutablePath.cpp
    core/InspectService.cpp
    core/JobManager.cpp
    core/LibraryAccess.cpp
    core/LibraryPaths.cpp
    core/DatabaseSchema.cpp
    core/LibraryColorOrder.cpp
    core/MosaicOutput.cpp
    core/OutputPlan.cpp
    core/MosaicPlanning.cpp
    core/MosaicSampling.cpp
    core/LibraryVisuals.cpp
    core/PreviewEvaluation.cpp
    core/MosaicMetrics.cpp
    core/NeighborhoodState.cpp
    core/MosaicEngine.cpp
    core/MosaicSelection.cpp
    core/MosaicService.cpp
)
if (NOT MOSAICRAFT_CUDA)
    target_sources(mosaicraft_core PRIVATE core/CudaStubs.cpp)
endif()
include(cmake/PortableCompute.cmake)

target_compile_definitions(mosaicraft_core PRIVATE
    $<$<BOOL:${MOSAICRAFT_CUDA}>:MOSAICRAFT_CUDA>
)

target_include_directories(mosaicraft_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${TIFF_INCLUDE_DIR}
)

target_link_libraries(mosaicraft_core PUBLIC
    mosaicraft_archive
    ${OpenCV_LIBS}
    ${TIFF_LIBRARIES}
    PNG::PNG
    JPEG::JPEG
    Threads::Threads
)
if (WIN32)
    target_link_libraries(mosaicraft_core PRIVATE Psapi)
endif()
if (MOSAICRAFT_CUDA)
    target_include_directories(mosaicraft_core PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
    if (CMAKE_DL_LIBS)
        target_link_libraries(mosaicraft_core PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()
# sqlite3：根据 find_package 结果选择对应的 CMake target
if(TARGET unofficial::sqlite3::sqlite3)
    target_link_libraries(mosaicraft_core PUBLIC unofficial::sqlite3::sqlite3)
else()
    target_link_libraries(mosaicraft_core PUBLIC SQLite::SQLite3)
endif()

if (MOSAICRAFT_CUDA)
add_library(mosaicraft_cuda STATIC
    ${CUDA_OBJ1}
    ${CUDA_OBJ2}
    ${CUDA_OBJ3}
    ${CUDA_OBJ4}
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

# Windows 13.5 uses the hybrid loader from CUDA::cudart. Its driver runtime
# is not an SDK DLL dependency; Packaging.cmake deploys actual non-driver imports.

# ============================================================
# Web UI / HTTP API 入口（独立进程，复用 core 服务，保留旧命令兼容）
# ============================================================
add_executable(MosaicraftWebUI
    src/server.cpp
)
target_link_libraries(MosaicraftWebUI PRIVATE
    mosaicraft_core
    mosaicraft_cuda
)

include(cmake/Packaging.cmake)
target_include_directories(MosaicraftWebUI PRIVATE
    ${CMAKE_SOURCE_DIR}
)
# HTML 或脚本单独更新时也要同步到 exe 同目录。
add_custom_target(mosaicraft_webui_html
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MosaicraftWebUI>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/generated/index.html"
        "$<TARGET_FILE_DIR:MosaicraftWebUI>/index.html"
    VERBATIM
)
add_dependencies(MosaicraftWebUI mosaicraft_webui_html)

# ============================================================
# 单元测试 (doctest — 单头文件，跨平台)
# ============================================================
add_executable(mosaicraft_tests EXCLUDE_FROM_ALL
    tests/test_core.cpp
)
target_link_libraries(mosaicraft_tests PRIVATE
    mosaicraft_core
    mosaicraft_cuda
)
target_include_directories(mosaicraft_tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)

# CTest 集成
enable_testing()
add_test(NAME cuda_architecture_config COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_SOURCE_DIR}/tests/test_cuda_architectures.cmake")
add_test(NAME cuda_toolkit_config COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_SOURCE_DIR}/tests/test_cuda_toolkit.cmake")
add_test(NAME mosaicraft_tests COMMAND mosaicraft_tests)

add_executable(mosaicraft_regression_tests EXCLUDE_FROM_ALL
    tests/test_regression.cpp
)
target_link_libraries(mosaicraft_regression_tests PRIVATE
    mosaicraft_core
    mosaicraft_cuda
)
target_include_directories(mosaicraft_regression_tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
add_test(NAME mosaicraft_regression_tests COMMAND mosaicraft_regression_tests)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME portable_library COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/portable_library.py"
        --cli "$<TARGET_FILE:mosaicraft>")
    set_tests_properties(portable_library PROPERTIES TIMEOUT 120)
    add_test(NAME portable_project COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/portable_project.py"
        --cli "$<TARGET_FILE:mosaicraft>")
    set_tests_properties(portable_project PROPERTIES TIMEOUT 180)
    add_test(NAME study_paths COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_study_paths.py")
endif()

find_program(POWERSHELL_COMMAND NAMES pwsh powershell)
if (POWERSHELL_COMMAND)
    add_test(NAME cuda_build_info COMMAND "${POWERSHELL_COMMAND}" -NoProfile -File "${CMAKE_SOURCE_DIR}/tests/test_cuda_build_info.ps1")
    add_custom_target(mosaicraft_webui_smoke
        COMMAND "${POWERSHELL_COMMAND}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/webui_smoke.ps1"
            -WebUiExe "$<TARGET_FILE:MosaicraftWebUI>"
        DEPENDS MosaicraftWebUI mosaicraft
        USES_TERMINAL
        COMMENT "Running Web UI/API smoke test..."
    )
endif()

# 显式性能采样目标，不增加默认构建或 CTest 的负担。
add_executable(mosaicraft_ann_benchmark EXCLUDE_FROM_ALL scripts/benchmark_ann.cpp)
target_link_libraries(mosaicraft_ann_benchmark PRIVATE mosaicraft_core mosaicraft_cuda)

add_executable(mosaicraft_features_benchmark EXCLUDE_FROM_ALL scripts/benchmark_features.cpp)
target_link_libraries(mosaicraft_features_benchmark PRIVATE mosaicraft_core mosaicraft_cuda)

add_executable(mosaicraft_match_benchmark EXCLUDE_FROM_ALL scripts/benchmark_match.cpp)
target_link_libraries(mosaicraft_match_benchmark PRIVATE mosaicraft_core mosaicraft_cuda)

add_executable(mosaicraft_quality_benchmark EXCLUDE_FROM_ALL scripts/benchmark_quality.cpp)
target_link_libraries(mosaicraft_quality_benchmark PRIVATE mosaicraft_core mosaicraft_cuda)

add_executable(mosaicraft_cuda_replay EXCLUDE_FROM_ALL scripts/benchmark_cuda_replay.cpp)
target_link_libraries(mosaicraft_cuda_replay PRIVATE mosaicraft_core mosaicraft_cuda)

add_executable(mosaicraft_compatible_benchmark EXCLUDE_FROM_ALL scripts/benchmark_compatible.cpp)
target_link_libraries(mosaicraft_compatible_benchmark PRIVATE mosaicraft_core mosaicraft_cuda)

add_executable(mosaicraft_score_benchmark EXCLUDE_FROM_ALL scripts/benchmark_scoring.cpp)
target_link_libraries(mosaicraft_score_benchmark PRIVATE mosaicraft_core mosaicraft_cuda)

add_executable(mosaicraft_arch_benchmark EXCLUDE_FROM_ALL scripts/benchmark_cuda_architectures.cpp)
target_link_libraries(mosaicraft_arch_benchmark PRIVATE mosaicraft_core mosaicraft_cuda)
