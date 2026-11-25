cmake_minimum_required(VERSION 3.20)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/contracts.json" MOSAICRAFT_CONTRACTS_JSON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/contracts.json")
foreach(key IN ITEMS softwareVersion libraryVersion featureVersion apiVersion apiMajorVersion annVersion
        colorOrderVersion journalVersion studySchemaVersion checkpointPathsVersion uiStateVersion)
    string(JSON ${key} GET "${MOSAICRAFT_CONTRACTS_JSON}" ${key})
endforeach()
option(MOSAICRAFT_SMART_CROP_CUTLASS "Build the experimental Windows CUDA smart crop runtime" OFF)
if(MOSAICRAFT_SMART_CROP_CUTLASS)
    list(APPEND VCPKG_MANIFEST_FEATURES smart-crop-cutlass)
    if(NOT VCPKG_INSTALLED_DIR)
        set(VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg-cutlass" CACHE PATH "Isolated CUTLASS dependencies")
    endif()
endif()
if(CMAKE_HOST_WIN32 AND VCPKG_TARGET_TRIPLET MATCHES "^(x64|arm64)-windows-release$" AND NOT DEFINED VCPKG_HOST_TRIPLET)
    set(_vcpkg_host_triplet "$ENV{VCPKG_DEFAULT_HOST_TRIPLET}")
    if(NOT _vcpkg_host_triplet)
        include(cmake/WindowsVcpkg.cmake)
        mosaicraft_windows_host_architecture(_windows_host_arch)
        set(_vcpkg_host_triplet "${_windows_host_arch}-windows-release")
    endif()
    set(VCPKG_HOST_TRIPLET "${_vcpkg_host_triplet}" CACHE STRING "vcpkg host triplet")
endif()
project(Mosaicraft VERSION ${softwareVersion} LANGUAGES C CXX)
if(DEFINED MOSAICRAFT_LINUX_ARCH)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _linux_arch)
    if(_linux_arch MATCHES "^(x86_64|amd64)$")
        set(_linux_arch x64)
    elseif(_linux_arch STREQUAL "aarch64")
        set(_linux_arch arm64)
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT _linux_arch STREQUAL MOSAICRAFT_LINUX_ARCH)
        message(FATAL_ERROR "Preset linux-${MOSAICRAFT_LINUX_ARCH} needs a matching Linux target. "
            "Detected ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}; select a matching preset or pass a toolchain file.")
    endif()
endif()
configure_file(cmake/Contracts.h.in generated/MosaicraftContracts.h @ONLY)
set(commandShellScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/command-shell.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${commandShellScript}")
file(READ "${commandShellScript}" commandShellUiScript)
set(previewScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/preview.js")
set(previewPickerScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/preview-picker.js")
set(resultAnalysisScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/result-analysis.js")
set(outputPlanningScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/output-planning.js")
set(libraryVisualsScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/library-visuals.js")
set(previewInsightsScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/preview-insights.js")
set(taskMetricsScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/task-metrics.js")
set(comparisonScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/comparison.js")
set(localPathScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/local-path-browser.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${previewScript}" "${previewPickerScript}" "${resultAnalysisScript}" "${outputPlanningScript}" "${libraryVisualsScript}" "${previewInsightsScript}" "${taskMetricsScript}" "${comparisonScript}")
file(READ "${previewScript}" interactivePreviewScript)
file(READ "${previewPickerScript}" interactivePreviewPickerScript)
file(READ "${resultAnalysisScript}" taskResultAnalysisScript)
file(READ "${outputPlanningScript}" mosaicOutputPlanningScript)
file(READ "${libraryVisualsScript}" libraryVisualsUiScript)
file(READ "${previewInsightsScript}" mosaicPreviewInsightsScript)
file(READ "${taskMetricsScript}" taskMetricsUiScript)
file(READ "${comparisonScript}" comparisonUiScript)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${localPathScript}")
file(READ "${localPathScript}" localPathUiScript)
set(viewportScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/viewer-core.js")
set(portableScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/command-builder/portable-viewer.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${viewportScript}" "${portableScript}")
file(READ "${viewportScript}" mosaicViewportScript)
file(READ "${portableScript}" portableViewerScript)
configure_file(cmake/PortableViewer.h.in generated/PortableViewer.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cli-options.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cli-options.json" mosaicCliOptionsJson)
configure_file(cmake/CliOptionsData.h.in generated/CliOptionsData.h @ONLY)
# Keep the Web UI's presets in sync with the compiled CUDA dispatch table.
set(cudaTemplateSizeFile "${CMAKE_CURRENT_SOURCE_DIR}/compute/TemplateFeatureSizes.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cudaTemplateSizeFile}")
file(READ "${cudaTemplateSizeFile}" cudaTemplateSizeSource)
string(REGEX MATCHALL "X\\([0-9]+, *[0-9]+\\)" cudaTemplateSizeRows "${cudaTemplateSizeSource}")
set(cudaTemplateSizeValues)
foreach(row IN LISTS cudaTemplateSizeRows)
    string(REGEX REPLACE "X\\(([0-9]+), *([0-9]+)\\)" "\"\\1x\\2\"" size "${row}")
    list(APPEND cudaTemplateSizeValues "${size}")
endforeach()
string(JOIN ", " cudaTemplateSizesJson ${cudaTemplateSizeValues})
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

option(MOSAICRAFT_SMART_CROP "Enable OpenCV DNN smart image normalization" OFF)
option(MOSAICRAFT_SMART_CROP_CUDA "Enable native CUDA inference for smart crop (requires CUDA OpenCV)" OFF)
set(_opencv_components core imgproc imgcodecs)
if(MOSAICRAFT_SMART_CROP)
    list(APPEND _opencv_components dnn objdetect)
    set(_smart_crop_lock "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smart-crop-models.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_smart_crop_lock}")
    file(READ "${_smart_crop_lock}" MOSAICRAFT_SMART_CROP_MODELS_JSON)
    configure_file(cmake/SmartCropModels.h.in generated/SmartCropModels.h @ONLY)
endif()
# Use one OpenCV ABI for all build variants, including the optional DNN adapter.
find_package(OpenCV 5.1.0 EXACT REQUIRED CONFIG COMPONENTS ${_opencv_components})
if(MOSAICRAFT_SMART_CROP_CUTLASS)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$" OR
       NOT MOSAICRAFT_SMART_CROP_CUDA OR NOT OpenCV_MOSAICRAFT_DNN_CUTLASS OR
       NOT OpenCV_CUDA_VERSION VERSION_EQUAL "13.5.37" OR OpenCV_USE_CUDNN OR OpenCV_USE_CUBLAS)
        message(FATAL_ERROR "Use Windows x64 and the isolated opencv5[dnn-cuda-cutlass] build with CUDA 13.5.37")
    endif()
elseif(OpenCV_MOSAICRAFT_DNN_CUTLASS)
    message(FATAL_ERROR "The experimental OpenCV build requires MOSAICRAFT_SMART_CROP_CUTLASS")
elseif(MOSAICRAFT_SMART_CROP_CUDA AND (NOT OpenCV_CUDA_VERSION VERSION_EQUAL "13.5.37" OR
                                       NOT OpenCV_USE_CUDNN OR NOT OpenCV_CUDNN_VERSION VERSION_EQUAL "9.28.1"))
    message(FATAL_ERROR "Use the locked opencv5[dnn-cuda] build with CUDA 13.5.37 and cuDNN 9.28.1")
endif()
# sqlite3：优先 vcpkg (unofficial-sqlite3)，回退到系统包 (SQLite3)
find_package(unofficial-sqlite3 CONFIG QUIET)
if(NOT unofficial-sqlite3_FOUND)
    find_package(SQLite3 REQUIRED)
endif()
find_package(TIFF REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)
find_package(Threads REQUIRED)
# ============================================================
# CUDA 选项
# ============================================================
option(MOSAICRAFT_CUDA "Enable CUDA GPU acceleration (requires NVIDIA GPU + CUDA Toolkit)" "${WIN32}")

if(MOSAICRAFT_SMART_CROP_CUDA AND (NOT MOSAICRAFT_SMART_CROP OR NOT MOSAICRAFT_CUDA OR
    NOT CMAKE_SYSTEM_NAME MATCHES "^(Windows|Linux)$" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$"))
    message(FATAL_ERROR "MOSAICRAFT_SMART_CROP_CUDA requires MOSAICRAFT_SMART_CROP and MOSAICRAFT_CUDA on Windows or Linux x64")
endif()

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
    DEPENDS "${CMAKE_SOURCE_DIR}/compute/FastScore.cuh" "${CMAKE_SOURCE_DIR}/core/GridWeights.inc" "${CMAKE_BINARY_DIR}/cuda-architectures.txt" "${CMAKE_SOURCE_DIR}/compute/CudaBackend.cu" "${CMAKE_SOURCE_DIR}/compute/CudaBackend.h" "${CMAKE_SOURCE_DIR}/compute/DeviceBuffer.cuh" "${CMAKE_SOURCE_DIR}/compute/CudaPreflight.cuh"
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
    DEPENDS "${CMAKE_BINARY_DIR}/cuda-architectures.txt" "${CMAKE_SOURCE_DIR}/compute/FeatureExtractorCuda.cu" "${CMAKE_SOURCE_DIR}/compute/FeatureExtractorCuda.h" "${CMAKE_SOURCE_DIR}/compute/TileCanny.cuh" "${CMAKE_SOURCE_DIR}/compute/TileResize.cuh" "${CMAKE_SOURCE_DIR}/compute/CudaBackend.h" "${CMAKE_SOURCE_DIR}/compute/DeviceBuffer.cuh" "${CMAKE_SOURCE_DIR}/compute/CudaPreflight.cuh"
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
    DEPENDS "${CMAKE_BINARY_DIR}/cuda-architectures.txt" "${CMAKE_SOURCE_DIR}/compute/CompatibleFeatures.cu" "${CMAKE_SOURCE_DIR}/compute/CompatibleFeatures.h" "${CMAKE_SOURCE_DIR}/compute/TileResize.cuh"
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
    DEPENDS "${CMAKE_SOURCE_DIR}/compute/FastScore.cuh" "${CMAKE_BINARY_DIR}/cuda-architectures.txt" "${CMAKE_SOURCE_DIR}/compute/CompatibleScoring.cu" "${CMAKE_SOURCE_DIR}/compute/CompatibleScoring.h"
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
    core/SmartCropInference.cpp
    core/SmartCropSession.cpp
    core/LibraryPng.cpp
    core/BatchProcessor.cpp
    core/NormalizeService.cpp
    core/BuildService.cpp
    core/ColorOrder.cpp
    core/Database.cpp
    core/DatabaseService.cpp
    core/FeatureExtractor.cpp
    core/LibraryFeatureStore.cpp
    core/ExecutablePath.cpp
    core/InspectService.cpp
    core/JobManager.cpp
    core/LibraryAccess.cpp
    core/LibraryPaths.cpp
    core/LibraryMaintenance.cpp
    core/DatabaseSchema.cpp
    core/LibraryColorOrder.cpp
    core/MosaicOutput.cpp
    core/ParallelPngWriter.cpp
    core/OutputPlan.cpp
    core/MosaicPlanning.cpp
    core/MosaicSampling.cpp
    core/LibraryVisuals.cpp
    core/PreviewEvaluation.cpp
    core/MosaicMetrics.cpp
    core/NeighborhoodState.cpp
    core/TileFeatures.cpp
    core/MosaicEngine.cpp
    core/MosaicSelection.cpp
    core/PreviewCandidates.cpp
    core/PreviewCache.cpp
    core/MosaicService.cpp
)
if (NOT MOSAICRAFT_CUDA)
    target_sources(mosaicraft_core PRIVATE core/CudaStubs.cpp)
endif()
# 候选构建供真实 CLI 配对对照，验收前不改变发行版。
option(MOSAICRAFT_TRIAL_GENERIC_TARGET "Benchmark raw input for generic fast CUDA features" OFF)
option(MOSAICRAFT_TRIAL_CUDA_ORDERED_GRID "Diagnose template CUDA grid reduction order" OFF)
option(MOSAICRAFT_TRIAL_CUDA_TARGET "Benchmark tile-local resizing on the CUDA device" OFF)
option(MOSAICRAFT_TRIAL_SCHEDULE "Share the explicit worker budget across matching stages" OFF)
option(MOSAICRAFT_TRIAL_ANN_ROWS "Benchmark direct ANN batch rows" OFF)
option(MOSAICRAFT_TRIAL_CUDA_CANNY "Benchmark end-to-end CUDA target features" OFF)
option(MOSAICRAFT_TRIAL_CPU_TILES "Use verified reusable features for fast CPU tiles" ON)
option(MOSAICRAFT_TRIAL_LBP_SIMD "Vectorize LBP in the CPU tile trial" ON)
set_property(SOURCE core/MosaicEngine.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    MOSAICRAFT_TRIAL_GENERIC_TARGET=$<BOOL:${MOSAICRAFT_TRIAL_GENERIC_TARGET}>
    MOSAICRAFT_TRIAL_CUDA_ORDERED_GRID=$<BOOL:${MOSAICRAFT_TRIAL_CUDA_ORDERED_GRID}>
    MOSAICRAFT_TRIAL_CUDA_CANNY=$<BOOL:${MOSAICRAFT_TRIAL_CUDA_CANNY}>
    MOSAICRAFT_TRIAL_CUDA_TARGET=$<BOOL:${MOSAICRAFT_TRIAL_CUDA_TARGET}>
    MOSAICRAFT_TRIAL_SCHEDULE=$<BOOL:${MOSAICRAFT_TRIAL_SCHEDULE}>)
# 单格补算使用与整图 CPU 路径相同的特征和 ANN 试验开关。
set_property(SOURCE core/MosaicEngine.cpp core/PreviewCandidates.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    MOSAICRAFT_TRIAL_ANN_ROWS=$<BOOL:${MOSAICRAFT_TRIAL_ANN_ROWS}>
    MOSAICRAFT_TRIAL_CPU_TILES=$<BOOL:${MOSAICRAFT_TRIAL_CPU_TILES}>
    MOSAICRAFT_TRIAL_LBP_SIMD=$<BOOL:${MOSAICRAFT_TRIAL_LBP_SIMD}>)
option(MOSAICRAFT_TRIAL_TIFF_STRIPS "Group streamed TIFF rows in bounded strips" OFF)
option(MOSAICRAFT_TRIAL_PNG_FINAL_FLUSH "Only flush a streamed PNG at finalization" OFF)
option(MOSAICRAFT_TRIAL_PNG_SUB "Use fixed Sub filtering for speed-oriented streamed PNG" ON)
option(MOSAICRAFT_TRIAL_STREAM "Overlap explicit-budget JPEG/PNG encoding and placement" OFF)
set_property(SOURCE core/MosaicOutput.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    MOSAICRAFT_TRIAL_TIFF_STRIPS=$<BOOL:${MOSAICRAFT_TRIAL_TIFF_STRIPS}>
    MOSAICRAFT_TRIAL_PNG_FINAL_FLUSH=$<BOOL:${MOSAICRAFT_TRIAL_PNG_FINAL_FLUSH}>
    MOSAICRAFT_TRIAL_PNG_SUB=$<BOOL:${MOSAICRAFT_TRIAL_PNG_SUB}>
    MOSAICRAFT_TRIAL_STREAM=$<BOOL:${MOSAICRAFT_TRIAL_STREAM}>)
option(MOSAICRAFT_TRIAL_MATCH_READ "Read matching rows in the validated snapshot for small CPU creates" ON)
set_property(SOURCE core/MosaicEngine.cpp APPEND PROPERTY COMPILE_DEFINITIONS MOSAICRAFT_TRIAL_MATCH_READ=$<BOOL:${MOSAICRAFT_TRIAL_MATCH_READ}>)
option(MOSAICRAFT_TRIAL_LIBRARY_PATHS "Use validated sequential reads for the existing library format" ON)
set_property(SOURCE core/LibraryPaths.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    MOSAICRAFT_TRIAL_LIBRARY_PATHS=$<BOOL:${MOSAICRAFT_TRIAL_LIBRARY_PATHS}>)
option(MOSAICRAFT_TRIAL_BUILD_AHEAD "Benchmark bounded build normalization lookahead" OFF)
option(MOSAICRAFT_TRIAL_BUILD_HASH "Hash new CPU library inputs in producer slots" ON)
set_property(SOURCE core/BuildService.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    MOSAICRAFT_TRIAL_BUILD_AHEAD=$<BOOL:${MOSAICRAFT_TRIAL_BUILD_AHEAD}>
    MOSAICRAFT_TRIAL_BUILD_HASH=$<BOOL:${MOSAICRAFT_TRIAL_BUILD_HASH}>)
option(MOSAICRAFT_TRIAL_HOT_ANN "Reuse validated read-only CPU/CUDA library data across Web UI jobs" ON)
set_property(SOURCE core/JobManager.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    MOSAICRAFT_TRIAL_HOT_ANN=$<BOOL:${MOSAICRAFT_TRIAL_HOT_ANN}>)
include(cmake/PortableCompute.cmake)

target_compile_definitions(mosaicraft_core PRIVATE
    $<$<BOOL:${MOSAICRAFT_CUDA}>:MOSAICRAFT_CUDA>
)

if(MOSAICRAFT_SMART_CROP)
    target_sources(mosaicraft_core PRIVATE core/SmartCropGeometry.cpp)
endif()
if(MOSAICRAFT_SMART_CROP_CUDA)
    target_sources(mosaicraft_core PRIVATE core/SmartCropCudaRecovery.cpp)
endif()
target_compile_definitions(mosaicraft_core PRIVATE
    MOSAICRAFT_SMART_CROP=$<BOOL:${MOSAICRAFT_SMART_CROP}>
    MOSAICRAFT_SMART_CROP_CUDA=$<BOOL:${MOSAICRAFT_SMART_CROP_CUDA}>)

target_include_directories(mosaicraft_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hnswlib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann-json
    ${TIFF_INCLUDE_DIR}
)

target_link_libraries(mosaicraft_core PUBLIC
    mosaicraft_archive
    ${OpenCV_LIBS}
    ${TIFF_LIBRARIES}
    PNG::PNG
    ZLIB::ZLIB
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
option(MOSAICRAFT_BUILD_DESKTOP "Build the Qt Widgets desktop application" OFF)
option(MOSAICRAFT_DESKTOP_BACKEND_INSTALL "Install the desktop-capable CLI without the Web UI" OFF)
add_executable(mosaicraft
    src/main.cpp
)
set(_desktop_worker_enabled OFF)
if(MOSAICRAFT_BUILD_DESKTOP OR MOSAICRAFT_DESKTOP_BACKEND_INSTALL)
    set(_desktop_worker_enabled ON)
    target_sources(mosaicraft PRIVATE core/EditorSession.cpp)
    target_compile_definitions(mosaicraft PRIVATE MOSAICRAFT_DESKTOP_WORKER)
endif()

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
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
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
target_compile_definitions(mosaicraft_tests PRIVATE
    MOSAICRAFT_SMART_CROP=$<BOOL:${MOSAICRAFT_SMART_CROP}>
    MOSAICRAFT_SMART_CROP_CUDA=$<BOOL:${MOSAICRAFT_SMART_CROP_CUDA}>)
target_link_libraries(mosaicraft_tests PRIVATE
    mosaicraft_core
    mosaicraft_cuda
)
target_include_directories(mosaicraft_tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest
)

# CTest 集成
enable_testing()
if(MOSAICRAFT_BUILD_DESKTOP)
    add_subdirectory(desktop)
endif()
option(MOSAICRAFT_PYTHON_TESTS "Register optional Python integration and study tests" OFF)
add_test(NAME cuda_architecture_config COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_SOURCE_DIR}/tests/test_cuda_architectures.cmake")
add_test(NAME cuda_toolkit_config COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_SOURCE_DIR}/tests/test_cuda_toolkit.cmake")
add_test(NAME desktop_worker_command COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:mosaicraft>"
    "-DWORKER_ENABLED=${_desktop_worker_enabled}" -P "${CMAKE_SOURCE_DIR}/tests/test_desktop_worker.cmake")
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
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest
)
add_test(NAME mosaicraft_regression_tests COMMAND mosaicraft_regression_tests)
add_subdirectory(tools/release-check EXCLUDE_FROM_ALL)
option(TESTS "Include Mosaicraft test targets in the default build" OFF)
set(_test_targets_default)
if(TESTS)
    set(_test_targets_default ALL)
endif()
add_custom_target(mosaicraft_test_targets ${_test_targets_default} DEPENDS mosaicraft MosaicraftWebUI
    mosaicraft_tests mosaicraft_regression_tests mosaicraft_release_check)
add_test(NAME documentation_native COMMAND mosaicraft_release_check --docs "${CMAKE_SOURCE_DIR}")
add_test(NAME cuda_build_info_native COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_SOURCE_DIR}/tests/test_cuda_build_info.cmake")
add_test(NAME webui_native_smoke COMMAND mosaicraft_release_check
    "$<TARGET_FILE:MosaicraftWebUI>" "${CMAKE_SOURCE_DIR}/contracts.json" --current)
set_tests_properties(webui_native_smoke PROPERTIES TIMEOUT 240)
if(MOSAICRAFT_PYTHON_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME portable_library COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/portable_library.py"
        --cli "$<TARGET_FILE:mosaicraft>" --regression "$<TARGET_FILE:mosaicraft_regression_tests>")
    set_tests_properties(portable_library PROPERTIES TIMEOUT 120)
    add_test(NAME portable_project COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/portable_project.py"
        --cli "$<TARGET_FILE:mosaicraft>")
    set_tests_properties(portable_project PROPERTIES TIMEOUT 180)
    add_test(NAME study_paths COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_study_paths.py")
endif()

# 显式性能采样目标，不增加默认构建或 CTest 的负担。
add_executable(mosaicraft_ann_benchmark EXCLUDE_FROM_ALL scripts/benchmark_ann.cpp)
target_link_libraries(mosaicraft_ann_benchmark PRIVATE mosaicraft_core mosaicraft_cuda)

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
