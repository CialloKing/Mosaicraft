# The upstream opencv-python source distribution ships the native OpenCV sources.
# Extract only its OpenCV subtree; no wheel, Python build or bindings are used.
if("dnn-cuda" IN_LIST FEATURES)
    # The matching contrib source distribution also contains the required cudev headers.
    vcpkg_download_distfile(archive
        URLS "https://files.pythonhosted.org/packages/76/f7/52d9f0730e1e067461e6e91926d666b57ec8659ff530bf9e8a0bce309109/opencv_contrib_python_headless-5.1.0.95.tar.gz"
        FILENAME "opencv_contrib_python_headless-5.1.0.95.tar.gz"
        SHA512 6ac405b261dc2ff2cc019cdee16efc1ec3c473d76d296eaf93630713cd1c25ef1d50648c2193ad2d4b42338aeafa552606c4a773302fc925e1649b35fc11a64b
    )
    vcpkg_extract_source_archive(source ARCHIVE "${archive}" PATCHES face-detector-dnn-graph.patch smart-crop-cuda-operators.patch)
    set(extra_modules "${source}/opencv_contrib/modules/cudev")
else()
    vcpkg_download_distfile(archive
        URLS "https://files.pythonhosted.org/packages/6b/51/33b8c7255f3bdd920d6238aa478faf1e2136d5a51f17d826c13902ca7195/opencv_python_headless-5.1.0.95.tar.gz"
        FILENAME "opencv_python_headless-5.1.0.95.tar.gz"
        SHA512 09628a35dd5f80207dc2a4a18e75ca377ebc7634d538a362a18116d7bdde2ddaf417c331bba21fa668ab3f0a9f85dc160b17e6e78555266fb99b402f0d174194
    )
    vcpkg_extract_source_archive(source ARCHIVE "${archive}")
endif()
set(source "${source}/opencv")

set(modules core,imgproc,imgcodecs)
set(dnn OFF)
if("dnn" IN_LIST FEATURES)
    string(APPEND modules ",dnn,objdetect")
    set(dnn ON)
endif()

# Use the locked vcpkg WebP libraries on Windows as well as on Unix, never a bundled copy.
vcpkg_replace_string("${source}/cmake/OpenCVFindWebP.cmake" "NAMES webp)" "NAMES webp libwebp)")
vcpkg_replace_string("${source}/cmake/OpenCVFindWebP.cmake" "NAMES webpmux)" "NAMES webpmux libwebpmux)")
vcpkg_replace_string("${source}/cmake/OpenCVFindWebP.cmake" "NAMES webpdemux)" "NAMES webpdemux libwebpdemux)")

string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" static_crt)
string(COMPARE EQUAL "${VCPKG_TARGET_ARCHITECTURE}" "arm64" aarch64)
string(COMPARE EQUAL "${VCPKG_TARGET_ARCHITECTURE}" "x64" x64)
set(cuda OFF)
set(cuda_options)
if("dnn-cuda" IN_LIST FEATURES)
    if(NOT x64 OR (NOT VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_LINUX))
        message(FATAL_ERROR "CUDA DNN is available only for Windows and Linux x64")
    endif()
    set(cuda_root "$ENV{MOSAICRAFT_CUDA_SDK}")
    set(cudnn_root "$ENV{MOSAICRAFT_CUDNN_SDK}")
    if(NOT IS_ABSOLUTE "${cuda_root}" OR NOT IS_ABSOLUTE "${cudnn_root}")
        message(FATAL_ERROR "Prepare the locked CUDA and cuDNN SDKs before installing opencv5[dnn-cuda]")
    endif()
    if(VCPKG_TARGET_IS_WINDOWS)
        set(nvcc "${cuda_root}/bin/nvcc.exe")
        set(cudnn_library "${cudnn_root}/lib/x64/cudnn.lib")
    else()
        set(nvcc "${cuda_root}/bin/nvcc")
        set(cudnn_library "${cudnn_root}/lib/libcudnn.so")
    endif()
    foreach(file IN ITEMS "${nvcc}" "${cudnn_library}" "${cudnn_root}/include/cudnn_version.h")
        if(NOT EXISTS "${file}")
            message(FATAL_ERROR "Missing CUDA DNN build dependency: ${file}")
        endif()
    endforeach()
    execute_process(COMMAND "${nvcc}" --version RESULT_VARIABLE nvcc_status OUTPUT_VARIABLE nvcc_version)
    file(STRINGS "${cudnn_root}/include/cudnn_version.h" cudnn_version REGEX "^#define CUDNN_(MAJOR|MINOR|PATCHLEVEL) +[0-9]+")
    if(NOT nvcc_status EQUAL 0 OR NOT nvcc_version MATCHES "V13[.]5[.]37([ ,\r\n]|$)" OR
       NOT cudnn_version MATCHES "CUDNN_MAJOR +9(;|$)" OR NOT cudnn_version MATCHES "CUDNN_MINOR +28(;|$)" OR
       NOT cudnn_version MATCHES "CUDNN_PATCHLEVEL +1(;|$)")
        message(FATAL_ERROR "CUDA DNN requires nvcc 13.5.37 and cuDNN 9.28.1")
    endif()
    set(arches "$ENV{MOSAICRAFT_OPENCV_CUDA_ARCH}")
    if(NOT arches)
        set(arches "7.5 8.0 8.6 8.9 9.0 10.0 12.0")
    endif()
    if(NOT arches MATCHES "^[0-9]+[.][0-9]+( [0-9]+[.][0-9]+)*$")
        message(FATAL_ERROR "MOSAICRAFT_OPENCV_CUDA_ARCH must be a space-separated list such as 8.9 9.0")
    endif()
    # Cache only exact SDK locations; searching PATH may pick a different toolkit.
    set(cuda ON)
    string(APPEND modules ",cudev")
    # WITH_CUDNNJIT satisfies upstream config verification; it is ignored when cuDNN is present.
    list(APPEND cuda_options "-DOPENCV_EXTRA_MODULES_PATH=${extra_modules}"
        -DWITH_NVCUVID=OFF -DWITH_NVCUVENC=OFF -DWITH_CUFFT=OFF
        -DENABLE_CUDA_FIRST_CLASS_LANGUAGE=ON -DWITH_CUDNNJIT=ON
        "-DCUDAToolkit_ROOT=${cuda_root}" "-DCMAKE_CUDA_COMPILER=${nvcc}" "-DCUDNN_LIBRARY=${cudnn_library}"
        "-DCUDNN_INCLUDE_DIR=${cudnn_root}/include" "-DCUDA_ARCH_BIN=${arches}" -DCUDA_ARCH_PTX=12.0
        -DCMAKE_CUDA_RUNTIME_LIBRARY=Shared)
endif()
vcpkg_cmake_configure(SOURCE_PATH "${source}" OPTIONS
    "-DBUILD_LIST=${modules}"
    -DCMAKE_CXX_STANDARD=17
    -DOPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION=ON "-DAARCH64=${aarch64}" "-DX86_64=${x64}"
    -DOPENCV_SKIP_CMAKE_ROOT_CONFIG=ON -DOPENCV_CONFIG_INSTALL_PATH=share/opencv5
    -DOPENCV_INCLUDE_INSTALL_PATH=include/opencv5 -DOPENCV_LIB_INSTALL_PATH=lib -DOPENCV_BIN_INSTALL_PATH=bin
    -DOPENCV_OTHER_INSTALL_PATH=share/opencv5 -DOPENCV_LICENSES_INSTALL_PATH=share/opencv5/licenses
    -DOPENCV_GENERATE_SETUPVARS=OFF
    -DOPENCV_DLLVERSION=5 -DOPENCV_DEBUG_POSTFIX=d "-DBUILD_WITH_STATIC_CRT=${static_crt}"
    -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_PACKAGE=OFF
    -DBUILD_opencv_apps=OFF -DBUILD_opencv_python3=OFF -DBUILD_JAVA=OFF -DCV_TRACE=OFF
    -DCV_ENABLE_INTRINSICS=ON -DOPENCV_ENABLE_THREAD_SUPPORT=ON
    -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_CAROTENE=OFF -DWITH_KLEIDICV=OFF
    -DWITH_OPENCL=OFF "-DWITH_CUDA=${cuda}" "-DWITH_CUDNN=${cuda}" "-DOPENCV_DNN_CUDA=${cuda}"
    ${cuda_options}
    -DWITH_OPENCLAMDFFT=OFF -DWITH_OPENCLAMDBLAS=OFF -DWITH_OPENCL_D3D11_NV=OFF
    -DWITH_DIRECTX=OFF -DWITH_DIRECTML=OFF -DWITH_WIN32UI=OFF -DWITH_GTK=OFF
    -DWITH_DSHOW=OFF -DWITH_MSMF=OFF -DWITH_MSMF_DXVA=OFF -DWITH_V4L=OFF -DWITH_AVFOUNDATION=OFF
    -DWITH_EIGEN=OFF -DWITH_LAPACK=OFF -DWITH_HARFBUZZ=OFF -DWITH_VTK=OFF
    -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF -DWITH_OBSENSOR=OFF
    -DWITH_AVIF=OFF -DWITH_JASPER=OFF -DWITH_OPENEXR=OFF -DWITH_OPENJPEG=OFF -DWITH_SPNG=OFF
    -DWITH_JPEG=ON -DWITH_PNG=ON -DWITH_TIFF=ON -DWITH_WEBP=ON
    -DBUILD_JPEG=OFF -DBUILD_PNG=OFF -DBUILD_TIFF=OFF -DBUILD_WEBP=OFF -DBUILD_ZLIB=OFF
    "-DWITH_PROTOBUF=${dnn}" "-DPROTOBUF_UPDATE_FILES=${dnn}" -DBUILD_PROTOBUF=OFF
    -DWITH_FLATBUFFERS=OFF -DWITH_ONNXRUNTIME=OFF
    -DENABLE_CONFIG_VERIFICATION=ON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup()
vcpkg_copy_pdbs()
# OpenCV installs notices for bundled code such as SoftFloat separately.
file(GLOB_RECURSE notices LIST_DIRECTORIES FALSE "${CURRENT_PACKAGES_DIR}/share/opencv5/licenses/*")
if(cuda)
    list(APPEND notices "${source}/../opencv_contrib/LICENSE")
endif()
vcpkg_install_copyright(FILE_LIST "${source}/LICENSE" ${notices})
file(REMOVE "${CURRENT_PACKAGES_DIR}/LICENSE")
