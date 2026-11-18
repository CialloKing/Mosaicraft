option(MOSAICRAFT_VULKAN "Build optional dynamically loaded Vulkan compute backend" OFF)
option(MOSAICRAFT_METAL "Build candidate Metal backend on macOS" OFF)
target_sources(mosaicraft_core PRIVATE compute/ComputeBackend.cpp compute/PortableCompute.cpp)
# ISA 选项只用于各自的归约文件，分派本身必须能在基线 CPU 上运行。
set(_score_arch "${CMAKE_SYSTEM_PROCESSOR};${CMAKE_GENERATOR_PLATFORM}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(_score_arch "${CMAKE_OSX_ARCHITECTURES}")
endif()
string(TOLOWER "${_score_arch}" _score_arch)
if(_score_arch MATCHES "(x86_64|amd64|x64)")
    target_sources(mosaicraft_core PRIVATE compute/FastDistance.cpp compute/FastDistance512.cpp)
    set_property(SOURCE compute/PortableCompute.cpp compute/FastDistance.cpp APPEND PROPERTY COMPILE_DEFINITIONS MOSAICRAFT_AVX2_DISTANCE)
    if(MSVC)
        set_property(SOURCE compute/FastDistance.cpp APPEND PROPERTY COMPILE_OPTIONS /arch:AVX2 /fp:precise)
        set_property(SOURCE compute/FastDistance512.cpp APPEND PROPERTY COMPILE_OPTIONS /arch:AVX512 /fp:precise)
    else()
        set_property(SOURCE compute/FastDistance.cpp APPEND PROPERTY COMPILE_OPTIONS -mavx2 -ffp-contract=off)
        set_property(SOURCE compute/FastDistance512.cpp APPEND PROPERTY COMPILE_OPTIONS -mavx512f -mavx512bw -mavx512dq -ffp-contract=off)
    endif()
elseif(_score_arch MATCHES "(aarch64|arm64)" AND NOT MSVC)
    target_sources(mosaicraft_core PRIVATE compute/FastDistance.cpp)
    set_property(SOURCE compute/PortableCompute.cpp APPEND PROPERTY COMPILE_DEFINITIONS MOSAICRAFT_NEON_DISTANCE)
    set_property(SOURCE compute/PortableCompute.cpp compute/FastDistance.cpp APPEND PROPERTY COMPILE_OPTIONS -ffp-contract=off)
endif()
if(MOSAICRAFT_VULKAN OR MOSAICRAFT_METAL)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
if(MOSAICRAFT_VULKAN)
    find_path(MOSAICRAFT_VULKAN_INCLUDE vulkan/vulkan.h HINTS "$ENV{MOSAICRAFT_VULKAN_INCLUDE}" "$ENV{VULKAN_SDK}/Include")
    if(NOT MOSAICRAFT_VULKAN_INCLUDE)
        message(FATAL_ERROR "Vulkan headers required; set MOSAICRAFT_VULKAN_INCLUDE (runtime loader is not linked)")
    endif()
    if(NOT MOSAICRAFT_GLSLANG AND DEFINED ENV{MOSAICRAFT_GLSLANG})
        set(MOSAICRAFT_GLSLANG "$ENV{MOSAICRAFT_GLSLANG}" CACHE FILEPATH "Offline GLSL compiler")
    endif()
    find_program(MOSAICRAFT_GLSLANG NAMES glslang glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin")
    if(NOT MOSAICRAFT_GLSLANG)
        message(FATAL_ERROR "glslang is required to build offline SPIR-V")
    endif()
    foreach(shader IN ITEMS features scoring)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/${shader}.spv"
            COMMAND "${MOSAICRAFT_GLSLANG}" -V --target-env vulkan1.1
                "-I${CMAKE_CURRENT_SOURCE_DIR}/core" -o "${CMAKE_CURRENT_BINARY_DIR}/generated/${shader}.spv"
                "${CMAKE_CURRENT_SOURCE_DIR}/compute/shaders/${shader}.comp"
            DEPENDS "compute/shaders/${shader}.comp" core/GridWeights.inc VERBATIM)
    endforeach()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/PortableSpirv.h"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_spirv.py"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/PortableSpirv.h"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/features.spv" "${CMAKE_CURRENT_BINARY_DIR}/generated/scoring.spv"
        DEPENDS scripts/embed_spirv.py "${CMAKE_CURRENT_BINARY_DIR}/generated/features.spv"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/scoring.spv" VERBATIM)
    target_sources(mosaicraft_core PRIVATE compute/VulkanBackend.cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/PortableSpirv.h")
    target_include_directories(mosaicraft_core PRIVATE "${MOSAICRAFT_VULKAN_INCLUDE}")
    target_compile_definitions(mosaicraft_core PRIVATE MOSAICRAFT_VULKAN)
    target_link_libraries(mosaicraft_core PRIVATE ${CMAKE_DL_LIBS})
endif()
if(MOSAICRAFT_METAL)
    if(NOT APPLE)
        message(FATAL_ERROR "Metal requires macOS")
    endif()
    enable_language(OBJCXX)
    set_target_properties(mosaicraft_core PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
    find_program(MOSAICRAFT_XCRUN xcrun REQUIRED)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/PortableMetal.h"
        COMMAND "${MOSAICRAFT_XCRUN}" -sdk macosx metal -std=metal3.0 -fno-fast-math -ffp-contract=off
            "-I${CMAKE_CURRENT_SOURCE_DIR}/core" -c "${CMAKE_CURRENT_SOURCE_DIR}/compute/shaders/portable.metal"
            -o "${CMAKE_CURRENT_BINARY_DIR}/generated/portable.air"
        COMMAND "${MOSAICRAFT_XCRUN}" -sdk macosx metallib "${CMAKE_CURRENT_BINARY_DIR}/generated/portable.air"
            -o "${CMAKE_CURRENT_BINARY_DIR}/generated/portable.metallib"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_metal.py"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/portable.metallib" "${CMAKE_CURRENT_BINARY_DIR}/generated/PortableMetal.h"
        DEPENDS compute/shaders/portable.metal core/GridWeights.inc scripts/embed_metal.py VERBATIM)
    target_sources(mosaicraft_core PRIVATE compute/MetalBackend.mm "${CMAKE_CURRENT_BINARY_DIR}/generated/PortableMetal.h")
    set_source_files_properties(compute/MetalBackend.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_compile_definitions(mosaicraft_core PRIVATE MOSAICRAFT_METAL)
    target_link_libraries(mosaicraft_core PRIVATE "-framework Metal" "-framework Foundation")
endif()
