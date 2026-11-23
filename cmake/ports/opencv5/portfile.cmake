# The upstream opencv-python source distribution ships the native OpenCV sources.
# Extract only its OpenCV subtree; no wheel, Python build or bindings are used.
vcpkg_download_distfile(archive
    URLS "https://files.pythonhosted.org/packages/6b/51/33b8c7255f3bdd920d6238aa478faf1e2136d5a51f17d826c13902ca7195/opencv_python_headless-5.1.0.95.tar.gz"
    FILENAME "opencv_python_headless-5.1.0.95.tar.gz"
    SHA512 09628a35dd5f80207dc2a4a18e75ca377ebc7634d538a362a18116d7bdde2ddaf417c331bba21fa668ab3f0a9f85dc160b17e6e78555266fb99b402f0d174194
)
vcpkg_extract_source_archive(source ARCHIVE "${archive}")
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
    -DWITH_OPENCL=OFF -DWITH_CUDA=OFF
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
vcpkg_install_copyright(FILE_LIST "${source}/LICENSE" ${notices})
file(REMOVE "${CURRENT_PACKAGES_DIR}/LICENSE")
