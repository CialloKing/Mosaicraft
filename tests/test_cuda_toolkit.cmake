# Exercise the cache boundary without requiring a CUDA installation in CPU CI.
set(root "${CMAKE_CURRENT_BINARY_DIR}/cuda-toolkit-guard")
file(MAKE_DIRECTORY "${root}/sdk" "${root}/sdk-other")
file(WRITE "${root}/sdk-other/component" "old SDK cache fixture\n")
foreach(variable IN ITEMS CUDAToolkit_NVCC_EXECUTABLE CUDAToolkit_BIN_DIR CUDA_TOOLKIT_INCLUDE CUDA_CUDART CUDA_cudart_LIBRARY)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DCUDAToolkit_ROOT=${root}/sdk"
        "-D${variable}=${root}/sdk-other/component" -P "${CMAKE_CURRENT_LIST_DIR}/../cmake/CudaToolkit.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result EQUAL 0 OR NOT error MATCHES "Mixed CUDA SDK/cache")
        message(FATAL_ERROR "Stale ${variable} was not rejected: ${output}${error}")
    endif()
endforeach()
