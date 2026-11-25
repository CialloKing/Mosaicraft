cmake_minimum_required(VERSION 3.25)
project(mosaicraft_smart_crop_vulkan_probe LANGUAGES CXX)

find_package(ncnn CONFIG REQUIRED)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NCNN_SHARED_LIB OR
   NOT NCNN_VULKAN OR NOT NCNN_VERSION STREQUAL "20261020")
    message(FATAL_ERROR "Expected the pinned static Windows x64 Vulkan-enabled ncnn SDK")
endif()

add_executable(smart_crop_vulkan_probe main.cpp)
target_compile_features(smart_crop_vulkan_probe PRIVATE cxx_std_17)
target_link_libraries(smart_crop_vulkan_probe PRIVATE ncnn)
